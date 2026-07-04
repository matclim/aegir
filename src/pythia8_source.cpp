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
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mc_particle_source.hpp"
#include "pythia_common.hpp"

namespace {

// ============================================================================
// Single-threaded Pythia8 source
// ============================================================================

class Pythia8Source : public phlex::source {
 public:
  Pythia8Source(std::string const& xml_dir, double beam_energy,
                std::string const& process) {
    pythia_ = std::make_unique<Pythia8::Pythia>(xml_dir, false);
    aegir::configure_beams(*pythia_, 2212, 2212, beam_energy);
    pythia_->readString(process + " = on");
    pythia_->readString("Print:quiet = on");
    pythia_->init();
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const&) {
    aegir::next_event(*pythia_, "Pythia8Source");
    return aegir::extract_particles<SHiP::MCParticle>(pythia_->event);
  }

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::serial);
  }

  phlex::index_generator indices() override { co_return; }

 private:
  std::unique_ptr<Pythia8::Pythia> pythia_;
};

// ============================================================================
// Multi-threaded Pythia8 source (PythiaParallel on dedicated thread)
// ============================================================================

class Pythia8MTSource : public phlex::source {
 public:
  Pythia8MTSource(std::string const& xml_dir, double beam_energy,
                  std::string const& process, int num_threads, long num_events,
                  std::size_t max_queue_size)
      : max_queue_size_{max_queue_size} {
    pythia_thread_ = std::jthread([=, this] {
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
        aegir::configure_beams(pythia, 2212, 2212, beam_energy);
        pythia.readString(process + " = on");
        pythia.readString("Print:quiet = on");
        pythia.readString("Parallelism:numThreads = " +
                          std::to_string(num_threads));
        pythia.init();

        ready_promise_.set_value();

        pythia.run(num_events, [this](Pythia8::Pythia* p) {
          auto particles = aegir::extract_particles<SHiP::MCParticle>(p->event);
          push(std::move(particles));
        });
      } catch (...) {
        auto ex = std::current_exception();
        {
          std::lock_guard lock{mutex_};
          worker_exception_ = ex;
        }
        // If init failed (before set_value) this reports it to the
        // constructor via ready_future_.get(); if run() failed (after
        // set_value) the promise is already satisfied and set_exception throws
        // future_error, which we ignore — pop() surfaces the stored exception
        // instead of exhaustion.
        try {
          ready_promise_.set_exception(ex);
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

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::serial);
  }

  phlex::index_generator indices() override { co_return; }

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
      // A failure inside the worker (init or pythia.run) is reported here so it
      // is not misattributed to exhaustion.
      if (worker_exception_) std::rethrow_exception(worker_exception_);
      // Exhaustion is a hard error rather than a silently-empty event: it means
      // the driver requested more events than the source's num_events. Failing
      // loudly stops empty entries being written to the output.
      throw std::runtime_error(
          "Pythia8MTSource: generation exhausted — the workflow requested more "
          "events than the source's configured num_events. Increase num_events "
          "or reduce the driver's event count.");
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
  // Set by the worker thread if init or generation throws; rethrown by pop() so
  // a real failure is not masked as generation exhaustion. Guarded by mutex_.
  std::exception_ptr worker_exception_;
  // std::jthread: joins on destruction, so a throw from ready_future_.get()
  // in the constructor unwinds cleanly instead of terminating the process.
  std::jthread pythia_thread_;
  std::promise<void> ready_promise_;
  std::future<void> ready_future_{ready_promise_.get_future()};
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
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
    s.add_source<Pythia8Source>("pythia8", xml_dir, beam_energy, process);
  } else {
    auto num_threads = config.get<int>("num_threads", 4);
    auto num_events = config.get<long>("num_events", 100);
    auto queue_size = config.get<int>("queue_size", 32);
    if (queue_size < 1)
      throw std::runtime_error("queue_size must be >= 1, got " +
                               std::to_string(queue_size));

    s.add_source<Pythia8MTSource>("pythia8", xml_dir, beam_energy, process,
                                  num_threads, num_events,
                                  static_cast<std::size_t>(queue_size));
  }
}
