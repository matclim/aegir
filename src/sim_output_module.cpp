// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// sim_output_module.cpp — Phlex module plugin
//
// Observers for simulation output:
//   - RNTuple parallel writer for MCParticles and SimResult
//   - Validation histograms

#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/global_control.h>
#include <spdlog/spdlog.h>

#include <ROOT/Hist/ConvertToTH1.hxx>
#include <ROOT/RFieldToken.hxx>
#include <ROOT/RFile.hxx>
#include <ROOT/RHist.hxx>
#include <ROOT/RHistConcurrentFiller.hxx>
#include <ROOT/RHistFillContext.hxx>
#include <ROOT/RNTupleFillContext.hxx>
#include <ROOT/RNTupleModel.hxx>
#include <ROOT/RNTupleParallelWriter.hxx>
#include <ROOT/RNTupleWriteOptions.hxx>
#include <SHiP/EventHeader.hpp>
#include <SHiP/MCParticle.hpp>
#include <SHiP/SimHit.hpp>
#include <SHiP/SimParticle.hpp>
#include <SHiP/SimResult.hpp>
#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "TH1D.h"
#include "math_utils.hpp"
#include "phlex/core/product_selector.hpp"
#include "phlex/module.hpp"

namespace {

using ROOT::REntry;
using ROOT::RNTupleModel;
using ROOT::RNTupleParallelWriter;
using ROOT::Experimental::RFile;
using ROOT::Experimental::RHist;
using ROOT::Experimental::RHistConcurrentFiller;
using ROOT::Experimental::RHistFillContext;

struct FillState {
  std::shared_ptr<ROOT::RNTupleFillContext> ctx;
  std::unique_ptr<REntry> entry;
  // Field tokens cached once per context to avoid a per-event name lookup.
  // Entries and tokens belong to their context's clone of the model, so they
  // must stay together with the context.
  ROOT::RFieldToken event_header;
  ROOT::RFieldToken mc_particles;
  ROOT::RFieldToken sim_hits;
  ROOT::RFieldToken sim_particles;
};

// Write options that bound per-context buffering. The stock defaults target
// ~400 MiB of write memory per fill context (128 MiB zipped cluster target,
// 1.28 GB max unzipped cluster), which multiplied across fill contexts made
// writer memory grow with the event count until every context saturated
// (issue #77).
ROOT::RNTupleWriteOptions make_write_options(std::size_t cluster_mib) {
  ROOT::RNTupleWriteOptions opts;
  std::size_t const cluster_bytes = cluster_mib * 1024 * 1024;
  opts.SetApproxZippedClusterSize(cluster_bytes);
  // The default max is 10x the approx target, but only at construction time —
  // keep the same ratio for the new target.
  opts.SetMaxUnzippedClusterSize(10 * cluster_bytes);
  return opts;
}

// Fixed-size pool of fill states. Bounds writer memory to
// n_contexts x per-context buffering, independent of the TBB thread count:
// per-thread storage would grow to one context per worker thread (56 on
// production hosts) as TBB migrates tasks. A fill context may be used from
// different threads as long as uses do not overlap; the queue pop/push
// provides that synchronization.
//
// When all states are leased, acquire() blocks the calling thread. That is
// deliberate: it is the writer's backpressure. Blocked write() calls occupy
// TBB worker threads, so when the writer falls behind, the framework runs
// out of threads to simulate further events and the source stalls — keeping
// the number of completed-but-unwritten events (whose products are pinned in
// memory) at ~thread count instead of growing with the event count.
class FillStatePool {
 public:
  template <typename Init>
  FillStatePool(RNTupleParallelWriter& writer, std::size_t n_contexts,
                Init init) {
    for (std::size_t i = 0; i < n_contexts; ++i) {
      auto state = std::make_unique<FillState>();
      state->ctx = writer.CreateFillContext();
      state->entry = state->ctx->CreateEntry();
      init(*state);
      states_.push(std::move(state));
    }
  }

  // RAII lease: the state returns to the pool even if Fill throws.
  class Lease {
   public:
    explicit Lease(FillStatePool& pool) : pool_{pool} {
      pool_.states_.pop(state_);
    }
    ~Lease() { pool_.states_.push(std::move(state_)); }
    Lease(Lease const&) = delete;
    Lease& operator=(Lease const&) = delete;
    FillState& operator*() const { return *state_; }

   private:
    FillStatePool& pool_;
    std::unique_ptr<FillState> state_;
  };

  Lease acquire() { return Lease{*this}; }

 private:
  tbb::concurrent_bounded_queue<std::unique_ptr<FillState>> states_;
};

using HistD = RHist<double>;
using FillerD = RHistConcurrentFiller<double>;
using ContextD = RHistFillContext<double>;

std::shared_ptr<HistD> make_hist(int nbins, double low, double high) {
  return std::make_shared<HistD>(static_cast<std::uint64_t>(nbins),
                                 std::make_pair(low, high));
}

// RNTuple writer for MC particles only (used when no G4 simulation)
class MCRNTupleWriter {
 public:
  MCRNTupleWriter(std::string filename, std::size_t cluster_mib,
                  std::size_t n_contexts) {
    auto model = RNTupleModel::CreateBare();
    model->MakeField<SHiP::EventHeader>("event_header");
    model->MakeField<std::vector<SHiP::MCParticle>>("mc_particles");
    writer_ = RNTupleParallelWriter::Recreate(
        std::move(model), "events", filename, make_write_options(cluster_mib));
    pool_.emplace(*writer_, n_contexts, [](FillState& state) {
      state.event_header = state.entry->GetToken("event_header");
      state.mc_particles = state.entry->GetToken("mc_particles");
    });
    spdlog::info(
        "sim_output_module: writing '{}' with {} fill context(s), "
        "{} MiB cluster target",
        filename, n_contexts, cluster_mib);
  }

  void write(std::vector<SHiP::MCParticle> const& particles,
             SHiP::EventHeader const& header) {
    auto lease = pool_->acquire();
    auto& state = *lease;
    // Bind the inputs directly for the duration of Fill (which only reads
    // them), avoiding a full copy of the particle vector every event.
    state.entry->BindRawPtr(state.event_header,
                            const_cast<SHiP::EventHeader*>(&header));
    state.entry->BindRawPtr(
        state.mc_particles,
        const_cast<std::vector<SHiP::MCParticle>*>(&particles));
    state.ctx->Fill(*state.entry);
  }

 private:
  // Member order matters: the pool (holding all fill contexts) must be
  // destroyed before the writer, which commits the dataset in its destructor
  // ("all fill contexts must be destroyed before CommitDataset() is called").
  std::unique_ptr<RNTupleParallelWriter> writer_;
  std::optional<FillStatePool> pool_;
};

// RNTuple writer for full simulation output (MC + G4 hits)
class SimRNTupleWriter {
 public:
  SimRNTupleWriter(std::string filename, bool filter_empty,
                   std::size_t cluster_mib, std::size_t n_contexts)
      : filter_empty_{filter_empty} {
    auto model = RNTupleModel::CreateBare();
    model->MakeField<SHiP::EventHeader>("event_header");
    model->MakeField<std::vector<SHiP::MCParticle>>("mc_particles");
    model->MakeField<std::vector<SHiP::SimHit>>("sim_hits");
    model->MakeField<std::vector<SHiP::SimParticle>>("sim_particles");
    writer_ = RNTupleParallelWriter::Recreate(
        std::move(model), "events", filename, make_write_options(cluster_mib));
    pool_.emplace(*writer_, n_contexts, [](FillState& state) {
      state.event_header = state.entry->GetToken("event_header");
      state.mc_particles = state.entry->GetToken("mc_particles");
      state.sim_hits = state.entry->GetToken("sim_hits");
      state.sim_particles = state.entry->GetToken("sim_particles");
    });
    spdlog::info(
        "sim_output_module: writing '{}' with {} fill context(s), "
        "{} MiB cluster target",
        filename, n_contexts, cluster_mib);
  }

  void write(std::vector<SHiP::MCParticle> const& particles,
             SHiP::SimResult const& result,
             SHiP::EventHeader const& header) {
    // The header is dropped with the rest of the event, never on its own —
    // a header written without its particles would desynchronise the fields.
    if (filter_empty_ && result.hits.empty()) return;

    auto lease = pool_->acquire();
    auto& state = *lease;
    // Bind inputs directly for the duration of Fill (read-only), avoiding a
    // full copy of each vector every event.
    state.entry->BindRawPtr(state.event_header,
                            const_cast<SHiP::EventHeader*>(&header));
    state.entry->BindRawPtr(
        state.mc_particles,
        const_cast<std::vector<SHiP::MCParticle>*>(&particles));
    state.entry->BindRawPtr(
        state.sim_hits, const_cast<std::vector<SHiP::SimHit>*>(&result.hits));
    state.entry->BindRawPtr(
        state.sim_particles,
        const_cast<std::vector<SHiP::SimParticle>*>(&result.particles));
    state.ctx->Fill(*state.entry);
  }

 private:
  bool filter_empty_;
  // Member order matters: the pool (holding all fill contexts) must be
  // destroyed before the writer, which commits the dataset in its destructor
  // ("all fill contexts must be destroyed before CommitDataset() is called").
  std::unique_ptr<RNTupleParallelWriter> writer_;
  std::optional<FillStatePool> pool_;
};

// Validation histograms for MC particles (thread-safe via per-thread
// RHistFillContext atomic fillers)
class MCHistogrammer {
 public:
  explicit MCHistogrammer(std::string filename)
      : filename_{std::move(filename)},
        h_multiplicity_{make_hist(200, -0.5, 199.5)},
        h_momentum_{make_hist(200, 0., 500.)},
        // Range reaches ±2212 so protons/neutrons and hyperons land in real
        // bins rather than overflow; nuclei (10-digit codes) still overflow.
        h_pdg_{make_hist(5000, -2500.5, 2499.5)},
        f_multiplicity_{h_multiplicity_},
        f_momentum_{h_momentum_},
        f_pdg_{h_pdg_} {}

  void observe(std::vector<SHiP::MCParticle> const& particles) {
    auto& ctxs = fill_contexts_.local();
    if (!ctxs.multiplicity) {
      ctxs.multiplicity = f_multiplicity_.CreateFillContext();
      ctxs.momentum = f_momentum_.CreateFillContext();
      ctxs.pdg = f_pdg_.CreateFillContext();
    }
    ctxs.multiplicity->Fill(static_cast<double>(particles.size()));
    for (auto const& p : particles) {
      double pmag = aegir::magnitude(p.momentum);
      ctxs.momentum->Fill(pmag);
      ctxs.pdg->Fill(static_cast<double>(p.pdgCode));
    }
  }

  ~MCHistogrammer() {
    // Destroy per-thread fill contexts so their stats flush back into the
    // histograms before we read them, and so the concurrent fillers see no
    // live contexts at their own destruction (which would std::terminate).
    fill_contexts_.clear();
    try {
      auto file = RFile::Recreate(filename_);
      auto put = [&](char const* name, char const* title, HistD const& h) {
        auto th1 = ROOT::Experimental::Hist::ConvertToTH1D(h);
        th1->SetNameTitle(name, title);
        file->Put(name, *th1);
      };
      put("h_mc_multiplicity", "MC particles per event;N;Events",
          *h_multiplicity_);
      put("h_mc_momentum", "MC particle |p|;|p| [GeV/c];Entries", *h_momentum_);
      put("h_mc_pdg", "MC particle PDG code;PDG;Entries", *h_pdg_);
    } catch (std::exception const& e) {
      // RException, filesystem errors etc. — must not escape the destructor.
      try {
        spdlog::error(
            "sim_output_module: failed to write validation histograms to "
            "'{}': {}",
            filename_, e.what());
      } catch (...) {
      }
    }
  }

 private:
  struct FillContexts {
    std::shared_ptr<ContextD> multiplicity;
    std::shared_ptr<ContextD> momentum;
    std::shared_ptr<ContextD> pdg;
  };

  std::string filename_;
  std::shared_ptr<HistD> h_multiplicity_, h_momentum_, h_pdg_;
  FillerD f_multiplicity_, f_momentum_, f_pdg_;
  tbb::enumerable_thread_specific<FillContexts> fill_contexts_;
};

// Validation histograms for full simulation (thread-safe via per-thread
// RHistFillContext atomic fillers)
class SimHistogrammer {
 public:
  explicit SimHistogrammer(std::string filename)
      : filename_{std::move(filename)},
        h_mc_multiplicity_{make_hist(200, -0.5, 199.5)},
        h_mc_momentum_{make_hist(200, 0., 500.)},
        h_hit_multiplicity_{make_hist(1000, -0.5, 999.5)},
        h_hit_edep_{make_hist(200, 0., 1.0)},
        h_hit_z_{make_hist(200, -1000., 12000.)},
        h_hit_detector_{make_hist(100, -0.5, 99.5)},
        h_hit_momentum_{make_hist(200, 0., 500.)},
        h_particle_multiplicity_{make_hist(1000, -0.5, 999.5)},
        f_mc_multiplicity_{h_mc_multiplicity_},
        f_mc_momentum_{h_mc_momentum_},
        f_hit_multiplicity_{h_hit_multiplicity_},
        f_hit_edep_{h_hit_edep_},
        f_hit_z_{h_hit_z_},
        f_hit_detector_{h_hit_detector_},
        f_hit_momentum_{h_hit_momentum_},
        f_particle_multiplicity_{h_particle_multiplicity_} {}

  void observe(std::vector<SHiP::MCParticle> const& particles,
               SHiP::SimResult const& result) {
    auto& ctxs = fill_contexts_.local();
    if (!ctxs.mc_multiplicity) {
      ctxs.mc_multiplicity = f_mc_multiplicity_.CreateFillContext();
      ctxs.mc_momentum = f_mc_momentum_.CreateFillContext();
      ctxs.hit_multiplicity = f_hit_multiplicity_.CreateFillContext();
      ctxs.hit_edep = f_hit_edep_.CreateFillContext();
      ctxs.hit_z = f_hit_z_.CreateFillContext();
      ctxs.hit_detector = f_hit_detector_.CreateFillContext();
      ctxs.hit_momentum = f_hit_momentum_.CreateFillContext();
      ctxs.particle_multiplicity = f_particle_multiplicity_.CreateFillContext();
    }
    ctxs.mc_multiplicity->Fill(static_cast<double>(particles.size()));
    for (auto const& p : particles) {
      double pmag = aegir::magnitude(p.momentum);
      ctxs.mc_momentum->Fill(pmag);
    }
    ctxs.hit_multiplicity->Fill(static_cast<double>(result.hits.size()));
    for (auto const& hit : result.hits) {
      ctxs.hit_edep->Fill(hit.energyDeposit);
      ctxs.hit_z->Fill(hit.position[2]);
      ctxs.hit_detector->Fill(static_cast<double>(hit.detectorId));
      ctxs.hit_momentum->Fill(aegir::magnitude(hit.momentum));
    }
    ctxs.particle_multiplicity->Fill(
        static_cast<double>(result.particles.size()));
  }

  ~SimHistogrammer() {
    fill_contexts_.clear();
    try {
      auto file = RFile::Recreate(filename_);
      auto put = [&](char const* name, char const* title, HistD const& h) {
        auto th1 = ROOT::Experimental::Hist::ConvertToTH1D(h);
        th1->SetNameTitle(name, title);
        file->Put(name, *th1);
      };
      put("h_mc_multiplicity", "MC particles per event;N;Events",
          *h_mc_multiplicity_);
      put("h_mc_momentum", "MC particle |p|;|p| [GeV/c];Entries",
          *h_mc_momentum_);
      put("h_hit_multiplicity", "Sim hits per event;N;Events",
          *h_hit_multiplicity_);
      put("h_hit_edep", "Hit energy deposit;E_{dep} [GeV];Entries",
          *h_hit_edep_);
      put("h_hit_z", "Hit z position;z [mm];Entries", *h_hit_z_);
      put("h_hit_detector", "Hit detector ID;Detector ID;Entries",
          *h_hit_detector_);
      put("h_hit_momentum", "Hit |p|;|p| [GeV/c];Entries", *h_hit_momentum_);
      put("h_particle_multiplicity", "Sim particles per event;N;Events",
          *h_particle_multiplicity_);
    } catch (std::exception const& e) {
      try {
        spdlog::error(
            "sim_output_module: failed to write validation histograms to "
            "'{}': {}",
            filename_, e.what());
      } catch (...) {
      }
    }
  }

 private:
  struct FillContexts {
    std::shared_ptr<ContextD> mc_multiplicity;
    std::shared_ptr<ContextD> mc_momentum;
    std::shared_ptr<ContextD> hit_multiplicity;
    std::shared_ptr<ContextD> hit_edep;
    std::shared_ptr<ContextD> hit_z;
    std::shared_ptr<ContextD> hit_detector;
    std::shared_ptr<ContextD> hit_momentum;
    std::shared_ptr<ContextD> particle_multiplicity;
  };

  std::string filename_;
  std::shared_ptr<HistD> h_mc_multiplicity_, h_mc_momentum_;
  std::shared_ptr<HistD> h_hit_multiplicity_, h_hit_edep_, h_hit_z_,
      h_hit_detector_, h_hit_momentum_;
  std::shared_ptr<HistD> h_particle_multiplicity_;
  FillerD f_mc_multiplicity_, f_mc_momentum_;
  FillerD f_hit_multiplicity_, f_hit_edep_, f_hit_z_, f_hit_detector_,
      f_hit_momentum_;
  FillerD f_particle_multiplicity_;
  tbb::enumerable_thread_specific<FillContexts> fill_contexts_;
};

// No-op observers for benchmarking pure framework overhead.
class MCNoop {
 public:
  void observe(std::vector<SHiP::MCParticle> const&) {}
};

class SimNoop {
 public:
  void observe(std::vector<SHiP::MCParticle> const&, SHiP::SimResult const&) {}
};

}  // namespace

PHLEX_REGISTER_ALGORITHMS(m, config) {
  using namespace phlex;

  auto mode = config.get<std::string>("mode", std::string{"mc_only"});
  auto rntuple_file =
      config.get<std::string>("rntuple_file", std::string{"sim_output.root"});
  auto histo_file =
      config.get<std::string>("histo_file", std::string{"validation.root"});
  // Writer memory is bounded by fill_contexts x per-context buffering (which
  // scales with cluster_size_mib); see issue #77. The pool defaults to
  // min(4, framework threads): more contexts than threads can never fill
  // concurrently and would only multiply idle cluster buffers. phlex installs
  // its -j limit (a tbb::global_control) before loading modules, so the
  // active value here is the framework's thread count.
  auto const max_threads = tbb::global_control::active_value(
      tbb::global_control::max_allowed_parallelism);
  auto cluster_mib = config.get<int>("cluster_size_mib", 32);
  auto n_contexts = config.get<int>(
      "fill_contexts", static_cast<int>(std::min<std::size_t>(
                           4, std::max<std::size_t>(1, max_threads))));

  if (mode != "mc_only" && mode != "full" && mode != "noop" &&
      mode != "noop_full")
    throw std::runtime_error(
        "Unknown sim_output_module mode: '" + mode +
        "' (expected 'mc_only', 'full', 'noop', or 'noop_full')");
  if (cluster_mib <= 0 || n_contexts <= 0)
    throw std::runtime_error(
        "sim_output_module: cluster_size_mib and fill_contexts must be "
        "positive");

  if (mode == "noop") {
    auto noop = m.make<MCNoop>();
    noop.observe("noop", &MCNoop::observe, concurrency::unlimited)
        .input_family(product_selector{.creator = "mc_particles"_id,
                                       .layer = "event"_id});
  } else if (mode == "noop_full") {
    auto noop = m.make<SimNoop>();
    noop.observe("noop", &SimNoop::observe, concurrency::unlimited)
        .input_family(
            product_selector{.creator = "mc_particles"_id, .layer = "event"_id},
            product_selector{.creator = "geant4"_id,
                             .layer = "event"_id,
                             .suffix = "sim_result"_id});
  } else if (mode == "mc_only") {
    auto writer = m.make<MCRNTupleWriter>(rntuple_file,
                                          static_cast<std::size_t>(cluster_mib),
                                          static_cast<std::size_t>(n_contexts));
    // Unlimited on purpose: excess write() calls block on the pool lease,
    // which throttles the framework when the writer is the bottleneck. phlex
    // has no in-flight-event limit, so a concurrency-limited node would let
    // completed events queue in front of it without bound, each pinning its
    // products in memory (issue #77).
    writer
        .observe("write_rntuple", &MCRNTupleWriter::write,
                 concurrency::unlimited)
        .input_family(
            product_selector{.creator = "mc_particles"_id, .layer = "event"_id},
            product_selector{.creator = "event_header"_id,
                             .layer = "event"_id});

    auto histogrammer = m.make<MCHistogrammer>(histo_file);
    histogrammer
        .observe("validate", &MCHistogrammer::observe, concurrency::unlimited)
        .input_family(product_selector{.creator = "mc_particles"_id,
                                       .layer = "event"_id});
  } else {
    auto filter_empty = config.get<bool>("filter_empty", false);

    auto writer = m.make<SimRNTupleWriter>(
        rntuple_file, filter_empty, static_cast<std::size_t>(cluster_mib),
        static_cast<std::size_t>(n_contexts));
    // Unlimited on purpose: excess write() calls block on the pool lease,
    // which throttles the framework when the writer is the bottleneck. phlex
    // has no in-flight-event limit, so a concurrency-limited node would let
    // completed events queue in front of it without bound, each pinning its
    // products in memory (issue #77).
    writer
        .observe("write_rntuple", &SimRNTupleWriter::write,
                 concurrency::unlimited)
        .input_family(
            product_selector{.creator = "mc_particles"_id, .layer = "event"_id},
            product_selector{.creator = "geant4"_id,
                             .layer = "event"_id,
                             .suffix = "sim_result"_id},
            product_selector{.creator = "event_header"_id,
                             .layer = "event"_id});

    auto histogrammer = m.make<SimHistogrammer>(histo_file);
    histogrammer
        .observe("validate", &SimHistogrammer::observe, concurrency::unlimited)
        .input_family(
            product_selector{.creator = "mc_particles"_id, .layer = "event"_id},
            product_selector{.creator = "geant4"_id,
                             .layer = "event"_id,
                             .suffix = "sim_result"_id});
  }
}
