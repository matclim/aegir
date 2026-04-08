// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// pythia8_source.cpp — Phlex source plugin for Pythia8 event generation
//
// Provides MCParticle vectors from Pythia8 in 400 GeV fixed-target mode.
// Supports both single-threaded (serial Pythia) and multi-threaded
// (PythiaParallel) operation, selected via config.

#include <Pythia8/Pythia.h>
#include <Pythia8/PythiaParallel.h>

#include <SHiP/MCParticle.hpp>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "phlex/core/product_query.hpp"
#include "phlex/model/data_cell_index.hpp"
#include "phlex/source.hpp"

namespace {

// Extract final-state particles from a Pythia event record
std::vector<SHiP::MCParticle> extract_particles(Pythia8::Event const& event) {
  std::vector<SHiP::MCParticle> particles;
  particles.reserve(event.size());
  for (int i = 0; i < event.size(); ++i) {
    auto const& p = event[i];
    if (!p.isFinal()) continue;

    SHiP::MCParticle mc;
    mc.pdgCode = p.id();
    mc.vertex = {p.xProd(), p.yProd(), p.zProd()};  // mm
    mc.momentum = {p.px(), p.py(), p.pz()};         // GeV
    mc.energy = p.e();
    mc.time = p.tProd() / 299.792458;  // mm/c → ns
    mc.motherId = p.mother1();
    mc.status = p.statusHepMC();
    particles.push_back(mc);
  }
  return particles;
}

// ============================================================================
// Single-threaded Pythia8 source
// ============================================================================

class Pythia8Source {
 public:
  Pythia8Source(std::string const& xml_dir, double beam_energy,
                std::string const& process) {
    pythia_ = std::make_unique<Pythia8::Pythia>(xml_dir, false);
    pythia_->readString("Beams:idA = 2212");
    pythia_->readString("Beams:idB = 2212");
    pythia_->readString("Beams:frameType = 2");
    pythia_->readString("Beams:eA = " + std::to_string(beam_energy));
    pythia_->readString("Beams:eB = 0.");
    pythia_->readString(process + " = on");
    pythia_->readString("Print:quiet = on");
    pythia_->init();
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const&) {
    if (!pythia_->next())
      throw std::runtime_error("Pythia8 event generation failed");
    return extract_particles(pythia_->event);
  }

 private:
  std::unique_ptr<Pythia8::Pythia> pythia_;
};

// ============================================================================
// Multi-threaded Pythia8 source (PythiaParallel on dedicated thread)
// ============================================================================

class Pythia8MTSource {
 public:
  Pythia8MTSource(std::string const& xml_dir, double beam_energy,
                  std::string const& process, int num_threads, long num_events,
                  std::size_t max_queue_size)
      : max_queue_size_{max_queue_size} {
    pythia_thread_ = std::thread([=, this] {
      // Ensure done_ is always signalled when the thread exits,
      // whether by normal completion or exception.
      struct DoneGuard {
        Pythia8MTSource& self;
        ~DoneGuard() {
          std::lock_guard lock{self.mutex_};
          self.done_ = true;
          self.cv_pop_.notify_all();
          self.cv_push_.notify_all();
        }
      } guard{*this};

      try {
        Pythia8::PythiaParallel pythia(xml_dir, false);
        pythia.readString("Beams:idA = 2212");
        pythia.readString("Beams:idB = 2212");
        pythia.readString("Beams:frameType = 2");
        pythia.readString("Beams:eA = " + std::to_string(beam_energy));
        pythia.readString("Beams:eB = 0.");
        pythia.readString(process + " = on");
        pythia.readString("Print:quiet = on");
        pythia.readString("Parallelism:numThreads = " +
                          std::to_string(num_threads));
        pythia.init();

        ready_promise_.set_value();

        pythia.run(num_events, [this](Pythia8::Pythia* p) {
          auto particles = extract_particles(p->event);
          push(std::move(particles));
        });
      } catch (...) {
        // Only set exception if the promise has not yet been fulfilled
        // (i.e. failure during initialisation, before set_value).
        try {
          ready_promise_.set_exception(std::current_exception());
        } catch (std::future_error const&) {
        }
      }
    });

    ready_future_.get();
  }

  ~Pythia8MTSource() {
    {
      std::lock_guard lock{mutex_};
      done_ = true;
      cv_push_.notify_all();
      cv_pop_.notify_all();
    }
    if (pythia_thread_.joinable()) pythia_thread_.join();
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const&) {
    return pop();
  }

 private:
  void push(std::vector<SHiP::MCParticle> particles) {
    std::unique_lock lock{mutex_};
    cv_push_.wait(lock,
                  [this] { return queue_.size() < max_queue_size_ || done_; });
    if (done_) return;
    queue_.push(std::move(particles));
    cv_pop_.notify_one();
  }

  std::vector<SHiP::MCParticle> pop() {
    std::unique_lock lock{mutex_};
    cv_pop_.wait(lock, [this] { return !queue_.empty() || done_; });
    if (queue_.empty()) {
      spdlog::warn(
          "Pythia8MTSource: generation exhausted, returning empty event");
      return {};
    }
    auto result = std::move(queue_.front());
    queue_.pop();
    cv_push_.notify_one();
    return result;
  }

  std::size_t max_queue_size_;
  std::mutex mutex_;
  std::condition_variable cv_push_;
  std::condition_variable cv_pop_;
  std::queue<std::vector<SHiP::MCParticle>> queue_;
  bool done_ = false;
  std::thread pythia_thread_;
  std::promise<void> ready_promise_;
  std::future<void> ready_future_{ready_promise_.get_future()};
};

}  // namespace

PHLEX_REGISTER_PROVIDERS(s, config) {
  using namespace phlex;

  auto xml_dir = config.get<std::string>("xml_dir", [] {
    if (auto const* env = std::getenv("PYTHIA8DATA")) return std::string{env};
    return std::string{"../share/Pythia8/xmldoc"};
  }());
  auto beam_energy = config.get<double>("beam_energy", 400.0);
  auto process =
      config.get<std::string>("process", std::string{"SoftQCD:inelastic"});
  auto parallel = config.get<bool>("parallel", false);

  if (!parallel) {
    auto src = s.make<Pythia8Source>(xml_dir, beam_energy, process);
    src.provide("generate", &Pythia8Source::generate, concurrency::serial)
        .output_product(
            product_query{.creator = "mc_particles"_id, .layer = "event"_id});
  } else {
    auto num_threads = config.get<int>("num_threads", 4);
    auto num_events = config.get<long>("num_events", 100);
    auto queue_size = config.get<int>("queue_size", 32);
    if (queue_size < 1)
      throw std::runtime_error("queue_size must be >= 1, got " +
                               std::to_string(queue_size));

    auto src = s.make<Pythia8MTSource>(xml_dir, beam_energy, process,
                                       num_threads, num_events,
                                       static_cast<std::size_t>(queue_size));
    src.provide("generate", &Pythia8MTSource::generate, concurrency::serial)
        .output_product(
            product_query{.creator = "mc_particles"_id, .layer = "event"_id});
  }
}
