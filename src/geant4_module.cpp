// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geant4_module.cpp — Phlex module plugin for Geant4 simulation
//
// Phlex TBB threads act as G4 workers directly (no separate G4 thread pool,
// no event queue, no BeamOn). Each framework thread lazily initialises a
// G4WorkerRunManagerKernel and calls G4EventManager::ProcessOneEvent with a
// manually-built G4Event. Inspired by the CMSSW OscarMTProducer pattern.
//
// Architecture:
//   - Dedicated master std::thread owns G4MTRunManager (geometry/physics init)
//   - Per phlex-thread: G4WorkerRunManagerKernel + G4EventManager for tracking
//   - No G4VUserPrimaryGeneratorAction — events built directly from MCParticle

#include <spdlog/spdlog.h>

#include <G4Event.hh>
#include <G4EventManager.hh>
#include <G4MTRunManager.hh>
#include <G4ParticleDefinition.hh>
#include <G4ParticleTable.hh>
#include <G4PhysListFactory.hh>
#include <G4PrimaryParticle.hh>
#include <G4PrimaryVertex.hh>
#include <G4StateManager.hh>
#include <G4SystemOfUnits.hh>
#include <G4Threading.hh>
#include <G4TransportationManager.hh>
#include <G4UImanager.hh>
#include <G4VUserActionInitialization.hh>
#include <G4VUserPrimaryGeneratorAction.hh>
#include <G4WorkerRunManagerKernel.hh>
#include <G4WorkerThread.hh>
#include <SHiP/MCParticle.hpp>
#include <SHiP/SimResult.hpp>
#include <atomic>
#include <cmath>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "FieldService/IFieldSource.h"
#include "chrome_trace.hpp"
#include "detector_construction.hpp"
#include "geant4_sim_core.hpp"
#include "geometry_source.hpp"
#include "math_utils.hpp"
#include "phlex/core/product_selector.hpp"
#include "phlex/module.hpp"

namespace {

using namespace SHiP::g4;

// Required by G4MTRunManager::Initialize but never invoked — events are
// built directly and passed to G4EventManager::ProcessOneEvent.
class StubPrimaryGenerator : public G4VUserPrimaryGeneratorAction {
 public:
  void GeneratePrimaries(G4Event*) override {}
};

class DirectActionInit : public G4VUserActionInitialization {
 public:
  void BuildForMaster() const override {}
  void Build() const override { SetUserAction(new StubPrimaryGenerator()); }
};

// Per-thread G4 worker kernel (leaked at shutdown — G4 singleton teardown
// is unsafe)
thread_local G4WorkerRunManagerKernel* tl_kernel = nullptr;

// Per-thread PDG code → G4ParticleDefinition cache
thread_local std::unordered_map<int, G4ParticleDefinition*> tl_pdg_cache;

struct Geant4SimConfig {
  std::string physics_list = "FTFP_BERT";
  int verbosity = 0;
  int concurrency = 1;
  SDMode sd_mode = SDMode::scoring;
  double ke_threshold = 0.0;
  bool energy_cut = false;
  double energy_cut_threshold = 0.0;
  double particle_ke_cut = 0.0;
  std::vector<std::pair<std::string, double>> regions;
};

class Geant4Sim {
 public:
  explicit Geant4Sim(Geant4SimConfig cfg) : cfg_{std::move(cfg)} {}

  ~Geant4Sim() {
    shutdown_promise_.set_value();
    if (master_thread_.joinable()) master_thread_.join();
  }

  SHiP::SimResult simulate(std::shared_ptr<SHiP::IGeometrySource> const& geo,
                           std::shared_ptr<ship::IFieldSource> const& field,
                           std::vector<SHiP::MCParticle> const& particles) {
    AEGIR_TRACE_EVENT("g4", "simulate");

    std::call_once(init_flag_,
                   [this, &geo, &field]() { init_master(geo, field); });

    if (!tl_kernel) init_worker();

    tl_hits.clear();
    tl_particles.clear();
    tl_track_map.clear();

    // unique_ptr so a throw from ProcessOneEvent below can't leak the event
    // (and its primary vertices/particles); G4 does not take ownership.
    auto event = std::make_unique<G4Event>(next_event_id_.fetch_add(1));
    {
      AEGIR_TRACE_EVENT("g4", "build_primaries");
      for (auto const& mc : particles) {
        auto [it, inserted] = tl_pdg_cache.try_emplace(mc.pdgCode, nullptr);
        if (inserted)
          it->second =
              G4ParticleTable::GetParticleTable()->FindParticle(mc.pdgCode);
        auto* def = it->second;
        if (!def) continue;
        double pmag = aegir::magnitude(mc.momentum);
        if (pmag <= 0) continue;

        auto* vertex = new G4PrimaryVertex(mc.vertex[0] * mm, mc.vertex[1] * mm,
                                           mc.vertex[2] * mm, mc.time * ns);
        auto* particle =
            new G4PrimaryParticle(def, mc.momentum[0] * GeV,
                                  mc.momentum[1] * GeV, mc.momentum[2] * GeV);
        vertex->SetPrimary(particle);
        event->AddPrimaryVertex(vertex);
      }
    }

    // G4EventManager expects G4State_GeomClosed; it transitions to
    // G4State_EventProc internally and back when done.
    auto* state_mgr = G4StateManager::GetStateManager();
    state_mgr->SetNewState(G4State_GeomClosed);
    {
      AEGIR_TRACE_EVENT("g4", "ProcessOneEvent");
      tl_kernel->GetEventManager()->ProcessOneEvent(event.get());
    }
    state_mgr->SetNewState(G4State_GeomClosed);

    SHiP::SimResult result;
    {
      AEGIR_TRACE_EVENT("g4", "flush_hits");
      AEGIR_TRACE_COUNTER("g4", "hits", tl_hits.size());
      AEGIR_TRACE_COUNTER("g4", "particles", tl_particles.size());
      result.hits = std::move(tl_hits);
      result.particles = std::move(tl_particles);
    }
    return result;
  }

 private:
  void init_master(std::shared_ptr<SHiP::IGeometrySource> const& geo,
                   std::shared_ptr<ship::IFieldSource> const& field) {
    std::promise<void> ready_promise;
    auto ready_future = ready_promise.get_future();

    field_ = field;  // keep alive for the G4 run
    detector_ = new ConfigurableDetectorConstruction(
        *geo, *field, cfg_.sd_mode, cfg_.ke_threshold, cfg_.regions);

    master_thread_ = std::jthread([this, &ready_promise] {
      try {
        AEGIR_TRACE_THREAD_NAME("g4_master");
        {
          AEGIR_TRACE_EVENT("g4", "init_master");
          auto* rm = new G4MTRunManager();
          rm->SetNumberOfThreads(cfg_.concurrency);
          rm->SetUserInitialization(detector_);

          G4PhysListFactory factory;
          auto* physics = factory.GetReferencePhysList(cfg_.physics_list);
          if (!physics)
            throw std::runtime_error("Unknown physics list: " +
                                     cfg_.physics_list);
          rm->SetUserInitialization(physics);

          rm->SetUserInitialization(new DirectActionInit());

          auto* ui = G4UImanager::GetUIpointer();
          ui->ApplyCommand("/run/verbose " + std::to_string(cfg_.verbosity));
          ui->ApplyCommand("/event/verbose 0");
          ui->ApplyCommand("/tracking/verbose 0");

          rm->Initialize();
          rm->RunInitialization();

          world_pv_ = G4TransportationManager::GetTransportationManager()
                          ->GetNavigatorForTracking()
                          ->GetWorldVolume();
          physics_list_ = physics;
        }

        ready_promise.set_value();

        shutdown_future_.wait();
        // Intentionally leak rm (G4 singleton teardown is unsafe)
      } catch (...) {
        ready_promise.set_exception(std::current_exception());
      }
    });

    ready_future.get();

    spdlog::info("Geant4 direct simulation ready ({} worker slots)",
                 cfg_.concurrency);
  }

  void init_worker() {
    AEGIR_TRACE_EVENT("g4", "init_worker");
    int id = next_thread_id_.fetch_add(1);
    AEGIR_TRACE_THREAD_NAME("g4_worker_" + std::to_string(id));
    G4Threading::G4SetThreadId(id);
    G4WorkerThread::BuildGeometryAndPhysicsVector();

    tl_kernel = new G4WorkerRunManagerKernel();
    tl_kernel->WorkerDefineWorldVolume(world_pv_);

    physics_list_->InitializeWorker();
    tl_kernel->SetPhysics(physics_list_);
    tl_kernel->InitializePhysics();

    detector_->ConstructSDandField();

    auto* evt_mgr = tl_kernel->GetEventManager();
    evt_mgr->SetUserAction(new TrackingAction(cfg_.particle_ke_cut));
    if (cfg_.energy_cut)
      evt_mgr->SetUserAction(new EnergyCutAction(cfg_.energy_cut_threshold));

    tl_kernel->RunInitialization();
    G4StateManager::GetStateManager()->SetNewState(G4State_GeomClosed);
  }

  Geant4SimConfig cfg_;

  std::once_flag init_flag_;
  G4VPhysicalVolume* world_pv_ = nullptr;
  G4VUserPhysicsList* physics_list_ = nullptr;
  ConfigurableDetectorConstruction* detector_ = nullptr;  // owned by G4
  std::shared_ptr<ship::IFieldSource> field_;             // outlives G4 run
  std::atomic<int> next_thread_id_{0};
  std::atomic<int> next_event_id_{0};
  // std::jthread: joins on destruction and on move-assignment, so a failed
  // init_master (which throws from ready_future.get() without setting
  // init_flag_) can be safely retried without terminating on reassignment.
  std::jthread master_thread_;
  std::promise<void> shutdown_promise_;
  std::shared_future<void> shutdown_future_{shutdown_promise_.get_future()};
};

}  // namespace

PHLEX_REGISTER_ALGORITHMS(m, config) {
  using namespace phlex;

  auto sd_mode_str = config.get<std::string>("sd_mode", std::string{"scoring"});
  auto ke_threshold = config.get<double>("ke_threshold", 0.0);
  auto regions_map = config.get<std::map<std::string, double>>(
      "regions", std::map<std::string, double>{});

  Geant4SimConfig cfg{
      .physics_list =
          config.get<std::string>("physics_list", std::string{"FTFP_BERT"}),
      .verbosity = config.get<int>("verbosity", 0),
      .concurrency = config.get<int>("concurrency", 1),
      .sd_mode = sd_mode_str == "crossing" ? SDMode::crossing : SDMode::scoring,
      .ke_threshold = ke_threshold,
      .energy_cut = config.get<bool>("energy_cut", false),
      .energy_cut_threshold =
          config.get<double>("energy_cut_threshold", double{ke_threshold}),
      .particle_ke_cut = config.get<double>("particle_ke_cut", 0.0),
      .regions = {regions_map.begin(), regions_map.end()},
  };

  auto num_threads = cfg.concurrency;
  auto g4 = m.make<Geant4Sim>(std::move(cfg));

  g4.transform("simulate", &Geant4Sim::simulate,
               concurrency{static_cast<std::size_t>(num_threads)})
      .input_family(
          product_selector{.creator = "geometry"_id, .layer = "job"_id},
          product_selector{.creator = "field"_id, .layer = "job"_id},
          product_selector{.creator = "mc_particles"_id, .layer = "event"_id})
      .output_product_suffixes("sim_result");
}
