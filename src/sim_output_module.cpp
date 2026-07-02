// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// sim_output_module.cpp — Phlex module plugin
//
// Observers for simulation output:
//   - RNTuple parallel writer for MCParticles and SimResult
//   - Validation histograms

#include <oneapi/tbb/enumerable_thread_specific.h>
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
#include <SHiP/MCParticle.hpp>
#include <SHiP/SimHit.hpp>
#include <SHiP/SimParticle.hpp>
#include <SHiP/SimResult.hpp>
#include <exception>
#include <memory>
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
  // Field tokens cached once per thread to avoid a per-event name lookup.
  ROOT::RFieldToken mc_particles;
  ROOT::RFieldToken sim_hits;
  ROOT::RFieldToken sim_particles;
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
  explicit MCRNTupleWriter(std::string filename) {
    auto model = RNTupleModel::CreateBare();
    model->MakeField<std::vector<SHiP::MCParticle>>("mc_particles");
    writer_ =
        RNTupleParallelWriter::Recreate(std::move(model), "events", filename);
  }

  void write(std::vector<SHiP::MCParticle> const& particles) {
    auto& state = fill_states_.local();
    if (!state.ctx) {
      state.ctx = writer_->CreateFillContext();
      state.entry = state.ctx->CreateEntry();
      state.mc_particles = state.entry->GetToken("mc_particles");
    }
    // Bind the input directly for the duration of Fill (which only reads it),
    // avoiding a full copy of the particle vector every event.
    state.entry->BindRawPtr(
        state.mc_particles,
        const_cast<std::vector<SHiP::MCParticle>*>(&particles));
    state.ctx->Fill(*state.entry);
  }

 private:
  std::unique_ptr<RNTupleParallelWriter> writer_;
  tbb::enumerable_thread_specific<FillState> fill_states_;
};

// RNTuple writer for full simulation output (MC + G4 hits)
class SimRNTupleWriter {
 public:
  SimRNTupleWriter(std::string filename, bool filter_empty = false)
      : filter_empty_{filter_empty} {
    auto model = RNTupleModel::CreateBare();
    model->MakeField<std::vector<SHiP::MCParticle>>("mc_particles");
    model->MakeField<std::vector<SHiP::SimHit>>("sim_hits");
    model->MakeField<std::vector<SHiP::SimParticle>>("sim_particles");
    writer_ =
        RNTupleParallelWriter::Recreate(std::move(model), "events", filename);
  }

  void write(std::vector<SHiP::MCParticle> const& particles,
             SHiP::SimResult const& result) {
    if (filter_empty_ && result.hits.empty()) return;

    auto& state = fill_states_.local();
    if (!state.ctx) {
      state.ctx = writer_->CreateFillContext();
      state.entry = state.ctx->CreateEntry();
      state.mc_particles = state.entry->GetToken("mc_particles");
      state.sim_hits = state.entry->GetToken("sim_hits");
      state.sim_particles = state.entry->GetToken("sim_particles");
    }
    // Bind inputs directly for the duration of Fill (read-only), avoiding a
    // full copy of each vector every event.
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
  std::unique_ptr<RNTupleParallelWriter> writer_;
  tbb::enumerable_thread_specific<FillState> fill_states_;
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

  if (mode != "mc_only" && mode != "full" && mode != "noop" &&
      mode != "noop_full")
    throw std::runtime_error(
        "Unknown sim_output_module mode: '" + mode +
        "' (expected 'mc_only', 'full', 'noop', or 'noop_full')");

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
    auto writer = m.make<MCRNTupleWriter>(rntuple_file);
    writer
        .observe("write_rntuple", &MCRNTupleWriter::write,
                 concurrency::unlimited)
        .input_family(product_selector{.creator = "mc_particles"_id,
                                       .layer = "event"_id});

    auto histogrammer = m.make<MCHistogrammer>(histo_file);
    histogrammer
        .observe("validate", &MCHistogrammer::observe, concurrency::unlimited)
        .input_family(product_selector{.creator = "mc_particles"_id,
                                       .layer = "event"_id});
  } else {
    auto filter_empty = config.get<bool>("filter_empty", false);

    auto writer = m.make<SimRNTupleWriter>(rntuple_file, filter_empty);
    writer
        .observe("write_rntuple", &SimRNTupleWriter::write,
                 concurrency::unlimited)
        .input_family(
            product_selector{.creator = "mc_particles"_id, .layer = "event"_id},
            product_selector{.creator = "geant4"_id,
                             .layer = "event"_id,
                             .suffix = "sim_result"_id});

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
