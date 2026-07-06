// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// genie_reader_source.cpp — Phlex source plugin reading GENIE rootracker files
//
// Provides MCParticle vectors from a GENIE "rootracker" ntuple (produced with
// `gntpc -f rootracker`), a plain-ROOT format that keeps the full particle
// stack with status codes and mother links. Reading it needs no GENIE
// libraries, so event generation stays decoupled from the aegir build; see
// docs/genie.md for how to produce the input files.

#include <TFile.h>
#include <TLeaf.h>
#include <TTree.h>

#include <SHiP/MCParticle.hpp>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mc_particle_source.hpp"

namespace {

// GENIE GHepStatus: 1 = stable final state (trackable by Geant4).
constexpr int kStableFinalState = 1;

// Stock gntpc caps the StdHep arrays at 250 particles (kNPmax); the actual
// buffer size is taken from the file in case it was produced with a larger
// cap.
constexpr int kDefaultMaxParticles = 250;

class GenieReaderSource : public phlex::source {
 public:
  GenieReaderSource(std::string const& file, std::string const& tree_name,
                    long long first_entry)
      : file_name_{file}, first_entry_{first_entry} {
    file_.reset(TFile::Open(file.c_str(), "READ"));
    if (!file_ || file_->IsZombie())
      throw std::runtime_error("genie_reader_source: cannot open '" + file +
                               "'");
    tree_ = file_->Get<TTree>(tree_name.c_str());
    if (!tree_)
      throw std::runtime_error("genie_reader_source: no TTree '" + tree_name +
                               "' in '" + file + "'");

    // The StdHep branches are variable-length arrays indexed by StdHepN; size
    // the buffers from the count leaf's file-wide maximum so GetEntry can
    // never write past them.
    int capacity = kDefaultMaxParticles;
    if (auto* leaf = tree_->GetLeaf("StdHepPdg")) {
      if (auto* count = leaf->GetLeafCount())
        capacity = std::max(capacity, static_cast<int>(count->GetMaximum()));
    }
    pdg_.resize(capacity);
    status_.resize(capacity);
    first_mother_.resize(capacity);
    p4_.resize(4 * static_cast<std::size_t>(capacity));

    tree_->SetBranchStatus("*", false);
    enable_branch("EvtVtx", vtx_);
    enable_branch("StdHepN", &n_particles_);
    enable_branch("StdHepPdg", pdg_.data());
    enable_branch("StdHepStatus", status_.data());
    enable_branch("StdHepFm", first_mother_.data());
    enable_branch("StdHepP4", p4_.data());
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto const entry = first_entry_ + static_cast<long long>(id.number());
    if (entry >= tree_->GetEntries())
      throw std::runtime_error(
          "genie_reader_source: input exhausted — the workflow requested "
          "entry " +
          std::to_string(entry) + " but '" + file_name_ + "' holds only " +
          std::to_string(tree_->GetEntries()) +
          " events. Reduce the driver's event count or provide a larger "
          "file.");
    tree_->GetEntry(entry);

    auto const n = n_particles_;
    if (n < 0 || n > static_cast<int>(pdg_.size()))
      throw std::runtime_error("genie_reader_source: corrupt entry " +
                               std::to_string(entry) +
                               ": StdHepN = " + std::to_string(n));

    // The interaction vertex is per event (EvtVtx, SI units); the per-particle
    // StdHepX4 positions are nuclear-scale offsets and irrelevant for
    // tracking.
    std::array<double, 3> const vertex{vtx_[0] * 1e3, vtx_[1] * 1e3,
                                       vtx_[2] * 1e3};  // m -> mm
    double const time = vtx_[3] * 1e9;                  // s -> ns

    std::vector<SHiP::MCParticle> particles;
    particles.reserve(static_cast<std::size_t>(n));
    // StdHep-record index -> output index for written (final-state) particles.
    std::vector<int> out_index(static_cast<std::size_t>(n), -1);

    for (int i = 0; i < n; ++i) {
      if (status_[i] != kStableFinalState) continue;

      out_index[static_cast<std::size_t>(i)] =
          static_cast<int>(particles.size());

      SHiP::MCParticle mc;
      mc.pdgCode = pdg_[static_cast<std::size_t>(i)];
      mc.vertex = vertex;
      mc.momentum = {p4(i, 0), p4(i, 1), p4(i, 2)};  // GeV
      mc.energy = p4(i, 3);                          // GeV
      mc.time = time;
      mc.motherId = first_mother_[static_cast<std::size_t>(i)];  // remapped
      mc.status = 1;
      particles.push_back(mc);
    }

    // Remap motherId from the full StdHep record to the emitted collection, or
    // -1 when the mother was not itself written out — the common case, since
    // mothers of final-state particles (the neutrino, the struck nucleus,
    // intermediate states) are generally not final state.
    for (auto& mc : particles) {
      int const m = mc.motherId;
      mc.motherId =
          (m >= 0 && m < n) ? out_index[static_cast<std::size_t>(m)] : -1;
    }
    return particles;
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
  template <typename T>
  void enable_branch(std::string const& name, T* address) {
    if (!tree_->GetBranch(name.c_str()))
      throw std::runtime_error("genie_reader_source: branch '" + name +
                               "' missing from '" + file_name_ +
                               "' — is this a rootracker file (gntpc -f "
                               "rootracker)?");
    tree_->SetBranchStatus(name.c_str(), true);
    tree_->SetBranchAddress(name.c_str(), address);
  }

  double p4(int i, int k) const {
    return p4_[4 * static_cast<std::size_t>(i) + static_cast<std::size_t>(k)];
  }

  std::string file_name_;
  long long first_entry_;
  std::unique_ptr<TFile> file_;
  TTree* tree_ = nullptr;  // owned by file_

  int n_particles_ = 0;
  double vtx_[4] = {0, 0, 0, 0};  // x, y, z, t (SI: m, s)
  std::vector<int> pdg_;
  std::vector<int> status_;
  std::vector<int> first_mother_;
  std::vector<double> p4_;  // [n][4]: px, py, pz, E (GeV)
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  auto file = config.get<std::string>("file");
  auto tree = config.get<std::string>("tree", std::string{"gRooTracker"});
  auto first_entry = config.get<long>("first_entry", 0L);
  if (first_entry < 0)
    throw std::runtime_error("genie_reader_source: first_entry must be >= 0");

  s.add_source<GenieReaderSource>("genie_reader", file, tree,
                                  static_cast<long long>(first_entry));
}
