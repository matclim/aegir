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
//   - Master init (G4MTRunManager, geometry/physics) runs on the process-wide
//     SHiP::geometry_thread(), shared with every other Geant4 geometry user
//     (e.g. aegir-genie's GENIE geometry analyzer): Geant4 permits only one
//     geometry-creating thread per process (aegir-genie issue #11)
//   - Per phlex-thread: G4WorkerRunManagerKernel + G4EventManager for tracking
//   - No G4VUserPrimaryGeneratorAction — events built directly from MCParticle
//   - Physics parameters and UI commands from the configuration
//     (hadronic_parameters, hadronic_flags, developer_parameters,
//     developer_flags, commands) are applied on the master in PreInit, see
//     geant4_tuning.hpp and docs/physics_parameters.md

#include <Random123/philox.h>
#include <spdlog/spdlog.h>

#include <G4Event.hh>
#include <G4EventManager.hh>
#include <G4GDMLParser.hh>
#include <G4GeometryManager.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4MTRunManager.hh>
#include <G4ParallelWorldPhysics.hh>
#include <G4ParticleDefinition.hh>
#include <G4ParticleTable.hh>
#include <G4PhysListFactory.hh>
#include <G4PhysicalVolumeStore.hh>
#include <G4PrimaryParticle.hh>
#include <G4PrimaryVertex.hh>
#include <G4RegionStore.hh>
#include <G4RunManagerKernel.hh>
#include <G4SolidStore.hh>
#include <G4StateManager.hh>
#include <G4Threading.hh>
#include <G4TransportationManager.hh>
#include <G4UImanager.hh>
#include <G4VUserActionInitialization.hh>
#include <G4VUserPrimaryGeneratorAction.hh>
#include <G4WorkerRunManagerKernel.hh>
#include <G4WorkerThread.hh>
#include <Randomize.hh>
#include <SHiP/MCParticle.hpp>
#include <SHiP/SimResult.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SHiP/FieldService/IFieldSource.h"
#include "SHiP/GeometryService/GeometryThread.h"
#include "chrome_trace.hpp"
#include "detector_construction.hpp"
#include "geant4_sim_core.hpp"
#include "geant4_tuning.hpp"
#include "geometry_source.hpp"
#include "hepmc_status.hpp"
#include "math_utils.hpp"
#include "phlex/configuration.hpp"
#include "phlex/core/product_selector.hpp"
#include "phlex/model/handle.hpp"
#include "phlex/module.hpp"
#include "phlex/utilities/max_allowed_parallelism.hpp"
#include "plane_particles.hpp"
#include "scoring_planes.hpp"
#include "seed_config.hpp"
#include "units/clhep_bridge.hpp"
#include "units/config_units.hpp"

namespace {

using SHiP::aegir::g4::ConfigurableDetectorConstruction;
using SHiP::aegir::g4::EnergyCutAction;
using SHiP::aegir::g4::find_track;
using SHiP::aegir::g4::kScoringWorldName;
using SHiP::aegir::g4::persist_ancestry;
using SHiP::aegir::g4::prune_to_hit_ancestry;
using SHiP::aegir::g4::ScoringPlaneConfig;
using SHiP::aegir::g4::ScoringShape;
using SHiP::aegir::g4::SDMode;
using SHiP::aegir::g4::tl_hits;
using SHiP::aegir::g4::tl_lineage;
using SHiP::aegir::g4::tl_lineage_map;
using SHiP::aegir::g4::tl_particles;
using SHiP::aegir::g4::tl_recorded_tracks;
using SHiP::aegir::g4::tl_track_map;
using SHiP::aegir::g4::TrackingAction;
using SHiP::aegir::g4::validate_scoring_planes;

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

// Moving the results out of tl_hits/tl_particles donates their buffers, so
// without a reserve every event regrows the vectors from zero capacity.
// Track per-thread high-water marks and pre-reserve to that size.
thread_local std::size_t tl_hits_hwm = 0;
thread_local std::size_t tl_particles_hwm = 0;

// The plane_particles of the event just processed (empty if no track was
// marked), from the tracks a recording plane marked. `primary_sources` holds
// the index into `particles` of each primary, in the order they were added.
std::vector<SHiP::MCParticle> record_plane_particles(
    G4Event const& event, std::vector<std::size_t> const& primary_sources,
    std::vector<SHiP::MCParticle> const& particles) {
  if (tl_recorded_tracks.empty()) {
    return {};
  }
  AEGIR_TRACE_EVENT("g4", "plane_particles");
  // Geant4 numbered the primaries when it turned them into tracks; vertices
  // and their particles are in the order they were added.
  std::unordered_map<int, std::size_t> primary_source;
  std::size_t n = 0;
  for (auto const* vertex = event.GetPrimaryVertex(); vertex != nullptr;
       vertex = vertex->GetNext()) {
    for (auto const* primary = vertex->GetPrimary(); primary != nullptr;
         primary = primary->GetNext()) {
      primary_source.emplace(primary->GetTrackID(), primary_sources.at(n++));
    }
  }
  return SHiP::aegir::build_plane_particles(tl_recorded_tracks, find_track,
                                            primary_source, particles);
}

struct Geant4SimConfig {
  std::string physics_list = "FTFP_BERT";
  int verbosity = 0;
  int concurrency = 1;
  std::uint32_t seed = 0;
  SDMode sd_mode = SDMode::scoring;
  SHiP::Energy ke_threshold = SHiP::Energy::zero();
  bool energy_cut = false;
  SHiP::Energy energy_cut_threshold = SHiP::Energy::zero();
  SHiP::Energy particle_ke_cut = SHiP::Energy::zero();
  // Hand every primary to Geant4 regardless of its generator status.
  // Escape hatch for inputs whose `status` does not follow the HepMC
  // convention (see hepmc_status.hpp); by default only final-state
  // (status 1) entries are tracked.
  bool track_all_primaries = false;
  // keep_particles: 'hit_ancestry' — write only the particles that left a
  // hit and their ancestors, instead of every recorded particle.
  bool keep_hit_ancestry = false;
  std::vector<std::pair<std::string, SHiP::Length>> regions;
  std::vector<ScoringPlaneConfig> scoring_planes;
  // When set, write the constructed geometry to this GDML file after
  // initialisation (e.g. to feed the same geometry to external tools).
  std::string export_gdml;
  // Log a progress line every this many simulated events (0 disables).
  int progress_interval = 100;
  // Physics parameters and UI commands applied in PreInit (geant4_tuning.hpp).
  SHiP::aegir::Geant4Tuning tuning;
};

class Geant4Sim {
 public:
  explicit Geant4Sim(Geant4SimConfig cfg)
      : cfg_{std::move(cfg)},
        record_particles_{std::ranges::any_of(
            cfg_.scoring_planes,
            [](ScoringPlaneConfig const& p) { return p.record_particles; })} {}

  ~Geant4Sim() {
    if (!master_constructed_) {
      return;
    }
    if (int kernels = built_kernels_.load(); kernels > 0) {
      spdlog::info("geant4: {} worker kernel(s) built for {} configured slots",
                   kernels, cfg_.concurrency);
    }
    // Empty the geometry stores now, on the geometry thread. Their static
    // singletons' destructors run at process exit on the program's main
    // thread, which never initialised Geant4's thread-local split-class
    // data — so mass-deleting the still-registered volumes there
    // dereferences a null per-thread base (e.g. ~G4PVPlacement →
    // GetRotation → G4PVData) and segfaults (#68). The geometry thread ran
    // the master initialisation, so the same deletions are safe there, and
    // the stores' destructors are left with nothing to delete. The relative
    // order of the Clean() calls is immaterial: every CleanStore() locks
    // its objects before deleting them, which suppresses all cross-store
    // references (e.g. ~G4LogicalVolume skips its
    // fRegion->RemoveRootLogicalVolume backreference when locked).
    SHiP::geometry_thread().run([] {
      G4GeometryManager::GetInstance()->OpenGeometry();
      G4RegionStore::Clean();
      G4PhysicalVolumeStore::Clean();
      G4LogicalVolumeStore::Clean();
      G4SolidStore::Clean();
    });
    // The run manager is intentionally leaked (G4 singleton teardown is
    // unsafe).
  }

  // The simulation result, and the plane_particles replay record (empty
  // unless a scoring plane records particles; see plane_particles.hpp).
  std::tuple<SHiP::SimResult, std::vector<SHiP::MCParticle>> simulate(
      std::shared_ptr<SHiP::aegir::IGeometrySource> const& geo,
      std::shared_ptr<SHiP::IFieldSource> const& field,
      phlex::handle<std::vector<SHiP::MCParticle>> particles) {
    AEGIR_TRACE_EVENT("g4", "simulate");

    // A failed master init is sticky: retrying would construct a second
    // G4MTRunManager next to the leaked first one and die on Geant4's fatal
    // "constructed twice" abort, masking the original error. Store the
    // exception instead and rethrow it on this and every later call — phlex
    // logs it at shutdown and terminates the job cleanly.
    std::call_once(init_flag_, [this, &geo, &field] {
      try {
        init_master(geo, field);
      } catch (std::exception const& e) {
        spdlog::error("geant4_module: master initialisation failed: {}",
                      e.what());
        init_error_ = std::current_exception();
      } catch (...) {
        init_error_ = std::current_exception();
      }
    });
    if (init_error_) {
      std::rethrow_exception(init_error_);
    }

    if (!tl_kernel) {
      init_worker();
    }

    // Seed the calling worker's engine from the data-cell index so the
    // event↔RNG pairing does not depend on which thread processes which
    // event, or in what order — any -j then reproduces bitwise.
    auto const& index = particles.data_cell_index();
    seed_engine(index.hash());

    tl_hits.clear();
    tl_particles.clear();
    tl_track_map.clear();
    tl_recorded_tracks.clear();
    tl_lineage.clear();
    tl_lineage_map.clear();
    tl_hits.reserve(tl_hits_hwm);
    tl_particles.reserve(std::max(tl_particles_hwm, particles->size()));

    // unique_ptr so a throw from ProcessOneEvent below can't leak the event
    // (and its primary vertices/particles); G4 does not take ownership.
    // The event id is the index number within the parent layer — stable
    // across runs, unlike an arrival-order counter. It is globally unique
    // only under the current flat event-under-job layout (a nested layout
    // would repeat numbers across parents); the hash-based seed above is
    // what needs cross-hierarchy uniqueness. G4int is 32-bit, so the id
    // saturates past ~2^31 events — far beyond any single run here.
    auto event = std::make_unique<G4Event>(static_cast<G4int>(index.number()));
    // Index into *particles of each primary, in the order they are added.
    std::vector<std::size_t> primary_sources;
    {
      AEGIR_TRACE_EVENT("g4", "build_primaries");
      std::size_t unknown_pdg = 0;
      std::size_t no_momentum = 0;
      std::size_t not_final_state = 0;
      std::size_t history = 0;
      for (std::size_t i = 0; i < particles->size(); ++i) {
        auto const& mc = (*particles)[i];
        // History written by an earlier stage (plane_particles) is expected
        // input, not something to warn about.
        if (!cfg_.track_all_primaries &&
            SHiP::aegir::hepmc::is_simulation_history(mc.status)) {
          ++history;
          continue;
        }
        // Only entries the generator left undecayed are ours to track:
        // decayed (2), documentation (3), beam (4) and intermediate (11-200)
        // entries are the generator's own bookkeeping, and tracking them
        // alongside their daughters would double-count the event. Checked
        // before the PDG lookup so that record-keeping entries (strings,
        // diquarks, nuclei) do not also trip the warning below.
        if (!cfg_.track_all_primaries &&
            !SHiP::aegir::hepmc::is_final_state(mc.status)) {
          ++not_final_state;
          continue;
        }
        auto [it, inserted] = tl_pdg_cache.try_emplace(mc.pdg_code, nullptr);
        if (inserted) {
          it->second =
              G4ParticleTable::GetParticleTable()->FindParticle(mc.pdg_code);
          // Warn once per unseen code (per thread): FindParticle also returns
          // null for nuclear (10-digit) PDG codes, which would need
          // G4IonTable::GetIon() to resolve.
          if (!it->second) {
            spdlog::warn(
                "geant4_module: no G4 particle definition for PDG code {} — "
                "such particles are skipped",
                mc.pdg_code);
          }
        }
        auto const* def = it->second;
        if (!def) {
          ++unknown_pdg;
          continue;
        }
        auto const mom = SHiP::view::momentum(mc);
        if (SHiP::aegir::magnitude(mom) <= SHiP::Momentum::zero()) {
          ++no_momentum;
          continue;
        }

        auto const vtx = SHiP::view::vertex(mc);
        namespace cb = SHiP::aegir::clhep;
        auto* vertex =
            new G4PrimaryVertex(cb::g4(vtx[0]), cb::g4(vtx[1]), cb::g4(vtx[2]),
                                cb::g4(SHiP::view::time(mc)));
        auto* particle = new G4PrimaryParticle(def, cb::g4(mom[0]),
                                               cb::g4(mom[1]), cb::g4(mom[2]));
        vertex->SetPrimary(particle);
        event->AddPrimaryVertex(vertex);
        primary_sources.push_back(i);
      }
      if (history > 0) {
        spdlog::debug(
            "geant4_module: event {}: {} history entries (HepMC status 201) "
            "not tracked",
            event->GetEventID(), history);
      }
      if (auto const skipped = unknown_pdg + no_momentum + not_final_state;
          skipped > 0) {
        spdlog::warn(
            "geant4_module: event {}: skipped {} of {} primaries ({} unknown "
            "PDG, {} non-positive momentum, {} not final state (HepMC status "
            "!= 1)) — output mc_particles still contains them",
            event->GetEventID(), skipped, particles->size(), unknown_pdg,
            no_momentum, not_final_state);
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

    // Both before keep_hit_ancestry prunes tl_particles, which would leave
    // tl_track_map pointing at the wrong entries.
    auto plane_particles =
        record_plane_particles(*event, primary_sources, *particles);
    persist_ancestry(tl_recorded_tracks);

    event.reset();

    SHiP::SimResult result;
    {
      AEGIR_TRACE_EVENT("g4", "flush_hits");
      AEGIR_TRACE_COUNTER("g4", "hits", tl_hits.size());
      AEGIR_TRACE_COUNTER("g4", "particles", tl_particles.size());
      tl_hits_hwm = std::max(tl_hits_hwm, tl_hits.size());
      tl_particles_hwm = std::max(tl_particles_hwm, tl_particles.size());
      if (cfg_.keep_hit_ancestry) {
        prune_to_hit_ancestry(tl_particles, tl_hits);
      }
      result.hits = std::move(tl_hits);
      result.particles = std::move(tl_particles);
    }

    auto const done = completed_.fetch_add(1) + 1;
    if (cfg_.progress_interval > 0 &&
        done % static_cast<std::size_t>(cfg_.progress_interval) == 0) {
      auto const elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - sim_start_)
                               .count();
      spdlog::info("geant4_module: {} events simulated ({:.1f} events/s)", done,
                   elapsed > 0 ? done / elapsed : 0.0);
    }

    return {std::move(result), std::move(plane_particles)};
  }

 private:
  // Derive a per-data-cell engine seed with Philox, following the
  // counter-based convention of SHiP::random::PhiloxRng: the key selects the
  // stream (config seed + a G4-specific constant so detector simulation and
  // event generation draw uncorrelated sequences), the counter is the full
  // index-path hash (unique across hierarchy levels, unlike number()).
  void seed_engine(std::size_t index_hash) const {
    r123::Philox4x32 const philox;
    r123::Philox4x32::key_type const key{{cfg_.seed, 0x47345EEDu}};
    r123::Philox4x32::ctr_type const ctr{
        {static_cast<std::uint32_t>(index_hash),
         static_cast<std::uint32_t>(index_hash >> 32), 0, 0}};
    auto const out = philox(ctr, key);
    // >> 1 clears the sign bit so the value always fits a non-negative long
    // (it can be 0, which the engine accepts).
    auto const seed64 =
        (static_cast<std::uint64_t>(out[1]) << 32 | out[0]) >> 1;
    G4Random::getTheEngine()->setSeed(static_cast<long>(seed64), 0);
  }

  void init_master(std::shared_ptr<SHiP::aegir::IGeometrySource> const& geo,
                   std::shared_ptr<SHiP::IFieldSource> const& field) {
    // Validate the export target up front, before anything is constructed:
    // G4GDMLParser::Write aborts via G4Exception on some write failures, so
    // pre-check the cases we can to fail with a catchable, clear error
    // instead. Only the existing-file and missing-parent-directory cases are
    // validated here — other fatal write errors (e.g. a read-only directory)
    // remain Geant4's responsibility. Failing before the G4MTRunManager is
    // constructed keeps the failure cheap: nothing has been built yet.
    if (!cfg_.export_gdml.empty()) {
      if (std::filesystem::exists(cfg_.export_gdml)) {
        throw std::runtime_error(
            "geant4_module: export_gdml target '" + cfg_.export_gdml +
            "' already exists — remove it or choose another path");
      }
      auto const parent = std::filesystem::path(cfg_.export_gdml).parent_path();
      if (!parent.empty() && !std::filesystem::exists(parent)) {
        throw std::runtime_error(
            "geant4_module: export_gdml target directory '" + parent.string() +
            "' does not exist");
      }
    }

    // Validate the physics list before constructing anything. Like export_gdml
    // above, GetReferencePhysList raises a fatal G4Exception for an unknown
    // list, so query IsReferencePhysList here — before detector_ or the run
    // manager exist — to fail with a catchable, clear error instead of aborting
    // the process. Failing this early also keeps the failure cheap: nothing has
    // been built yet. G4PhysListFactory only consults a static name table (it
    // creates no geometry), so validating on the calling thread is safe.
    G4PhysListFactory const phys_factory;
    if (!phys_factory.IsReferencePhysList(cfg_.physics_list)) {
      throw std::runtime_error("Unknown physics list: " + cfg_.physics_list);
    }

    field_ = field;  // keep alive for the G4 run
    detector_ = new ConfigurableDetectorConstruction(
        *geo, *field, cfg_.sd_mode, cfg_.ke_threshold, cfg_.regions,
        cfg_.scoring_planes);

    // Master initialisation runs on the process-wide geometry thread: only
    // one thread per process may create Geant4 geometry (G4GeomSplitter's
    // slot count is shared while its data array is thread-local), and other
    // plugins — e.g. aegir-genie's GENIE geometry analyzer — create theirs
    // on this thread too (aegir-genie issue #11, Geant4 bug #2747). The call
    // blocks until initialisation is done; exceptions propagate to the
    // caller in simulate(), which records them as a sticky init failure.
    SHiP::geometry_thread().run([this] {
      AEGIR_TRACE_THREAD_NAME("g4_master");
      AEGIR_TRACE_EVENT("g4", "init_master");
      auto* rm = new G4MTRunManager();
      master_constructed_ = true;
      rm->SetNumberOfThreads(cfg_.concurrency);
      rm->SetUserInitialization(detector_);

      // Before the physics list exists: its constructors read the transition
      // energies, and G4HadronicParameters accepts changes only in PreInit.
      SHiP::aegir::apply_hadronic_parameters(cfg_.tuning);

      // Physics list already validated in init_master before the run manager
      // was constructed, so GetReferencePhysList won't hit its unknown-list
      // abort.
      G4PhysListFactory factory;
      auto* physics = factory.GetReferencePhysList(cfg_.physics_list);
      // Transport particles through the scoring planes' parallel world too.
      // Not a layered-mass world: the planes take on no material, the
      // detector geometry's material applies everywhere.
      if (detector_->has_scoring_planes()) {
        physics->RegisterPhysics(
            new G4ParallelWorldPhysics(kScoringWorldName,
                                       /*layeredMass=*/false));
      }
      rm->SetUserInitialization(physics);

      rm->SetUserInitialization(new DirectActionInit());

      auto* ui = G4UImanager::GetUIpointer();
      ui->ApplyCommand("/run/verbose " + std::to_string(cfg_.verbosity));
      ui->ApplyCommand("/event/verbose 0");
      ui->ApplyCommand("/tracking/verbose 0");

      // After the physics list exists (its messengers, e.g. the EM extra
      // physics', are created with it), before initialisation (PreInit).
      SHiP::aegir::apply_geant4_commands(cfg_.tuning);

      rm->Initialize();
      rm->RunInitialization();

      world_pv_ = G4TransportationManager::GetTransportationManager()
                      ->GetNavigatorForTracking()
                      ->GetWorldVolume();
      physics_list_ = physics;

      if (!cfg_.export_gdml.empty()) {
        // The target path was validated at the top of init_master, before the
        // run manager was constructed.
        G4GDMLParser parser;
        parser.Write(cfg_.export_gdml, world_pv_);
        spdlog::info("geant4_module: geometry exported to {}",
                     cfg_.export_gdml);
      }
      // The run manager stays alive (and leaked) for the whole process.
    });

    sim_start_ = std::chrono::steady_clock::now();

    spdlog::info("Geant4 direct simulation ready ({} worker slots)",
                 cfg_.concurrency);
  }

  void init_worker() {
    AEGIR_TRACE_EVENT("g4", "init_worker");
    int const id = next_thread_id_.fetch_add(1);
    // TBB does not promise that the same OS threads serve this transform
    // for the whole run, so more kernels than configured slots can appear
    // (each new thread that ever runs simulate builds one). Geant4 11
    // tolerates thread ids beyond SetNumberOfThreads in this direct-
    // injection flow, but each extra kernel costs memory and a fresh RNG
    // stream — make it visible instead of silent.
    if (id >= cfg_.concurrency) {
      spdlog::warn(
          "geant4: initialising worker kernel #{} beyond the {} configured "
          "slots (TBB thread churn)",
          id + 1, cfg_.concurrency);
    }
    AEGIR_TRACE_THREAD_NAME("g4_worker_" + std::to_string(id));
    G4Threading::G4SetThreadId(id);
    G4WorkerThread::BuildGeometryAndPhysicsVector();

    tl_kernel = new G4WorkerRunManagerKernel();
    // Also registers the master's parallel worlds (the scoring planes) with
    // this thread's transportation manager, before the physics list's
    // G4ParallelWorldProcess looks them up.
    tl_kernel->WorkerDefineWorldVolume(world_pv_);
    // As G4WorkerRunManager::InitializeGeometry does: with parallel worlds,
    // InitializePhysics must build the same coupled transportation as the
    // master's.
    tl_kernel->SetNumberOfParallelWorld(
        G4MTRunManager::GetMasterRunManagerKernel()
            ->GetNumberOfParallelWorld());

    physics_list_->InitializeWorker();
    tl_kernel->SetPhysics(physics_list_);
    tl_kernel->InitializePhysics();

    detector_->ConstructSDandField();
    detector_->ConstructParallelSD();

    auto* evt_mgr = tl_kernel->GetEventManager();
    evt_mgr->SetUserAction(
        new TrackingAction(cfg_.particle_ke_cut, record_particles_));
    if (cfg_.energy_cut) {
      evt_mgr->SetUserAction(new EnergyCutAction(cfg_.energy_cut_threshold));
    }

    tl_kernel->RunInitialization();
    G4StateManager::GetStateManager()->SetNewState(G4State_GeomClosed);

    // Count only here, once the kernel is fully built: next_thread_id_ is
    // bumped on entry (to allocate the thread id) and would overcount a worker
    // whose init_worker threw partway through.
    built_kernels_.fetch_add(1);
  }

  Geant4SimConfig cfg_;
  // Whether a scoring plane records particles for replay.
  bool record_particles_;

  std::once_flag init_flag_;
  // Written at most once inside the call_once body, read only after the
  // call_once returns — no further synchronisation needed.
  std::exception_ptr init_error_;
  G4VPhysicalVolume* world_pv_ = nullptr;
  G4VUserPhysicsList* physics_list_ = nullptr;
  ConfigurableDetectorConstruction* detector_ = nullptr;  // owned by G4
  std::shared_ptr<SHiP::IFieldSource> field_;             // outlives G4 run
  std::atomic<int> next_thread_id_{0};
  // Kernels that finished init_worker successfully (unlike next_thread_id_,
  // which counts allocated ids). Reported in the shutdown summary.
  std::atomic<int> built_kernels_{0};
  // Completed-event count (completion order, not event id, so the logged
  // count is monotonic) and the reference point for the average event rate.
  // sim_start_ is written in init_master and published by the call_once
  // every simulate call passes through first.
  std::atomic<std::size_t> completed_{0};
  std::chrono::steady_clock::time_point sim_start_;
  // Set on the geometry thread as soon as the G4MTRunManager exists. From
  // that point the geometry stores may hold volumes — even when a later init
  // step throws, detector construction has typically already registered them
  // — so the destructor must clean the stores on the geometry thread (#68)
  // for failed and successful inits alike. Guarding on full init success
  // instead would leave the stores populated after a failed init and their
  // static destructors would segfault at process exit.
  bool master_constructed_ = false;
};

[[noreturn]] void plane_error(std::string const& plane,
                              std::string const& what) {
  throw std::invalid_argument("geant4_module: scoring plane '" + plane +
                              "': " + what);
}

// Read a plane's `shape` and its sizes (mm). Each shape's sizes are required
// and read only for that shape; a size belonging to the other shape is an
// error rather than silently ignored.
void read_plane_shape(phlex::configuration const& entry,
                      ScoringPlaneConfig& plane) {
  namespace su = SHiP::units;
  auto const shape = entry.get<std::string>("shape", std::string{"box"});
  auto const keys = entry.keys();
  auto length = [&](char const* key) {
    auto const value = entry.get_if_present<double>(key);
    if (!value) {
      plane_error(plane.name, std::string{"missing '"} + key + "' (mm)");
    }
    return *value * su::mm;
  };
  auto reject = [&](std::initializer_list<char const*> other_shape_keys) {
    for (auto const* key : other_shape_keys) {
      if (std::ranges::find(keys, key) != keys.end()) {
        plane_error(plane.name, std::string{"'"} + key +
                                    "' does not apply to shape '" + shape +
                                    "'");
      }
    }
  };
  if (shape == "box") {
    plane.shape = ScoringShape::box;
    reject({"r_min", "r_max"});
    plane.half_x = length("half_x");
    plane.half_y = length("half_y");
  } else if (shape == "tube") {
    plane.shape = ScoringShape::tube;
    reject({"half_x", "half_y"});
    plane.r_min = length("r_min");
    plane.r_max = length("r_max");
  } else {
    plane_error(plane.name,
                "shape must be 'box' or 'tube', not '" + shape + "'");
  }
  plane.half_z = length("half_z");
}

// Read the `scoring_planes` list (see scoring_planes.hpp and
// docs/geant4_integration.md). Lengths are in mm, energies in GeV. Unknown
// keys are rejected so that a misspelt option (say, `kil`) cannot silently
// leave a plane with its default behaviour.
std::vector<ScoringPlaneConfig> read_scoring_planes(
    phlex::configuration const& config) {
  namespace su = SHiP::units;
  static std::unordered_set<std::string> const known_keys{
      "name",  "id",           "shape",  "half_x",          "half_y",
      "r_min", "r_max",        "half_z", "position",        "after_volume",
      "gap",   "ke_threshold", "kill",   "record_particles"};

  std::vector<ScoringPlaneConfig> planes;
  auto const entries = config.get_if_present<std::vector<phlex::configuration>>(
      "scoring_planes");
  if (!entries) {
    return planes;
  }
  for (std::size_t i = 0; i < entries->size(); ++i) {
    auto const& entry = (*entries)[i];
    ScoringPlaneConfig plane{
        .name = entry.get<std::string>("name", std::string{}),
    };
    // Checked first, so that every later error can name the plane.
    if (plane.name.empty()) {
      throw std::invalid_argument("geant4_module: scoring plane #" +
                                  std::to_string(i) +
                                  " (counting from 0) has no 'name'");
    }
    for (auto const& key : entry.keys()) {
      if (!known_keys.contains(key)) {
        plane_error(plane.name, "unknown key '" + key + "'");
      }
    }
    // Required, so that a plane's id is chosen explicitly and stays the same
    // when planes are added, removed or reordered.
    auto const id = entry.get_if_present<int>("id");
    if (!id) {
      plane_error(plane.name, "missing 'id'");
    }
    plane.id = *id;
    read_plane_shape(entry, plane);
    if (auto const pos =
            entry.get_if_present<std::vector<double>>("position")) {
      if (pos->size() != 3) {
        plane_error(plane.name, "position needs 3 values [x, y, z] (mm)");
      }
      plane.position =
          SHiP::vec_of<SHiP::Length>({(*pos)[0], (*pos)[1], (*pos)[2]});
    }
    plane.after_volume = entry.get<std::string>("after_volume", std::string{});
    plane.gap = SHiP::aegir::get_quantity(entry, "gap", 0.0 * su::mm);
    plane.ke_threshold =
        SHiP::aegir::get_quantity(entry, "ke_threshold", 0.0 * su::GeV);
    plane.kill = entry.get<bool>("kill", false);
    plane.record_particles = entry.get<bool>("record_particles", false);
    planes.push_back(std::move(plane));
  }
  // Fail at configuration time, before any Geant4 object exists; placement
  // problems that need the geometry are reported by Construct().
  validate_scoring_planes(planes);
  return planes;
}

}  // namespace

PHLEX_REGISTER_ALGORITHMS(m, config) {
  using namespace phlex;

  namespace su = SHiP::units;

  auto const sd_mode_str =
      config.get<std::string>("sd_mode", std::string{"scoring"});
  auto const ke_threshold =
      SHiP::aegir::get_quantity(config, "ke_threshold", 0.0 * su::GeV);
  auto const regions_map = config.get<std::map<std::string, double>>(
      "regions", std::map<std::string, double>{});
  std::vector<std::pair<std::string, SHiP::Length>> regions;
  regions.reserve(regions_map.size());
  for (auto const& [pattern, cut_mm] : regions_map) {
    regions.emplace_back(pattern, cut_mm * su::mm);
  }

  // Default to the framework's TBB parallelism (phlex -j) so G4 workers
  // match the threads that can actually run them. A configured value above
  // the framework parallelism starves the graph: the extra worker slots
  // never run concurrently but G4 still initialises kernels for them.
  auto const active_parallelism =
      static_cast<int>(detail::max_allowed_parallelism::active_value());

  auto const seed = SHiP::aegir::resolve_seed(config, "geant4");

  auto const keep_particles =
      config.get<std::string>("keep_particles", std::string{"all"});
  if (keep_particles != "all" && keep_particles != "hit_ancestry") {
    throw std::invalid_argument("geant4_module: unknown keep_particles '" +
                                keep_particles +
                                "' (expected 'all' or 'hit_ancestry')");
  }

  Geant4SimConfig cfg{
      .physics_list =
          config.get<std::string>("physics_list", std::string{"FTFP_BERT"}),
      .verbosity = config.get<int>("verbosity", 0),
      .concurrency = config.get<int>("concurrency", int{active_parallelism}),
      .seed = seed,
      .sd_mode = sd_mode_str == "crossing" ? SDMode::crossing : SDMode::scoring,
      .ke_threshold = ke_threshold,
      .energy_cut = config.get<bool>("energy_cut", false),
      .energy_cut_threshold = SHiP::aegir::get_quantity(
          config, "energy_cut_threshold", SHiP::Energy{ke_threshold}),
      .particle_ke_cut =
          SHiP::aegir::get_quantity(config, "particle_ke_cut", 0.0 * su::GeV),
      .track_all_primaries = config.get<bool>("track_all_primaries", false),
      .keep_hit_ancestry = keep_particles == "hit_ancestry",
      .regions = std::move(regions),
      .scoring_planes = read_scoring_planes(config),
      .export_gdml = config.get<std::string>("export_gdml", std::string{}),
      .progress_interval = config.get<int>("progress_interval", 100),
      .tuning = SHiP::aegir::read_geant4_tuning(config),
  };

  for (auto const& [name, value] : cfg.tuning.hadronic_parameters) {
    spdlog::info("geant4_module: hadronic_parameters.{} = {}", name, value);
  }
  for (auto const& [name, value] : cfg.tuning.hadronic_flags) {
    spdlog::info("geant4_module: hadronic_flags.{} = {}", name, value);
  }
  for (auto const& [name, value] : cfg.tuning.developer_parameters) {
    spdlog::info("geant4_module: developer_parameters.{} = {}", name, value);
  }
  for (auto const& [name, value] : cfg.tuning.developer_flags) {
    spdlog::info("geant4_module: developer_flags.{} = {}", name, value);
  }
  for (auto const& command : cfg.tuning.commands) {
    spdlog::info("geant4_module: command '{}'", command);
  }

  // The pruning needs every ancestor recorded: with a particle_ke_cut, a
  // skipped ancestor would leave its descendants' parent ids dangling.
  if (cfg.keep_hit_ancestry && cfg.particle_ke_cut > SHiP::Energy::zero()) {
    throw std::invalid_argument(
        "geant4_module: keep_particles 'hit_ancestry' needs every ancestor "
        "recorded; set particle_ke_cut to 0");
  }

  if (cfg.concurrency > active_parallelism) {
    spdlog::warn(
        "geant4 concurrency ({}) exceeds framework parallelism ({}); "
        "at most {} events run concurrently — raise phlex -j or lower "
        "the module's concurrency",
        cfg.concurrency, active_parallelism, active_parallelism);
  }

  if (cfg.track_all_primaries) {
    spdlog::warn(
        "geant4_module: track_all_primaries is set — primaries are tracked "
        "regardless of generator status, so decayed (2), documentation (3) "
        "and beam (4) entries in the input are tracked too and will "
        "double-count the event");
  }

  auto const num_threads = cfg.concurrency;
  auto g4 = m.make<Geant4Sim>(std::move(cfg));

  g4.transform("simulate", &Geant4Sim::simulate,
               concurrency{static_cast<std::size_t>(num_threads)})
      .input_family(
          product_selector{.creator = "geometry"_id, .layer = "job"_id},
          product_selector{.creator = "field"_id, .layer = "job"_id},
          product_selector{.creator = "mc_particles"_id, .layer = "event"_id})
      .output_product_suffixes("sim_result", "plane_particles");
}
