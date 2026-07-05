// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// file_particle_source.cpp — Phlex source plugin reading primaries from a file
//
// Reads the "mc_particles" collection from an existing RNTuple file and
// republishes it, one event per data cell, so Geant4 re-propagates the stored
// primaries through the geometry. The collection field deserialises directly
// into RVec<SHiP::MCParticle>, so no per-field assembly or unit handling is
// needed. Mirrors the structure of the other aegir sources.

#include <ROOT/RNTupleReader.hxx>
#include <ROOT/RVec.hxx>
#include <spdlog/spdlog.h>

#include <SHiP/MCParticle.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mc_particle_source.hpp"

namespace {

class FileParticleSource : public phlex::source {
 public:
  FileParticleSource(std::string const& file, std::string const& ntuple)
      : reader_{ROOT::RNTupleReader::Open(ntuple, file)},
        v_particles_{
            reader_->GetView<ROOT::RVec<SHiP::MCParticle>>("mc_particles")} {
    n_entries_ = reader_->GetNEntries();
    spdlog::info("FileParticleSource: opened {} with {} entries", file,
                 n_entries_);
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto entry = static_cast<std::uint64_t>(id.number());
    if (entry >= n_entries_) return {};
    auto const& v = v_particles_(entry);
    return std::vector<SHiP::MCParticle>(v.begin(), v.end());
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
  std::unique_ptr<ROOT::RNTupleReader> reader_;
  ROOT::RNTupleView<ROOT::RVec<SHiP::MCParticle>> v_particles_;
  std::uint64_t n_entries_{0};
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  using namespace phlex;

  auto file = config.get<std::string>("file");
  auto ntuple = config.get<std::string>("ntuple", "events");

  s.add_source<FileParticleSource>("file_particles", file, ntuple);
}
