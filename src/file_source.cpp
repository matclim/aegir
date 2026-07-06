// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// file_source.cpp — Phlex source plugin
//
// Reads pre-generated events from a ROOT RNTuple, one entry per event, and
// publishes them as the "mc_particles" product. Each entry holds a
// std::vector, so the number of particles per event is arbitrary. This fills
// the role of FairShip's MuonBackGenerator / TTreeGenerator and enables a
// file-driven particle gun.
//
// The RNTuple layout matches what sim_output_module writes: a tuple (default
// name "events") with a std::vector<SHiP::MCParticle> field "mc_particles"
// and, in full-simulation output, a std::vector<SHiP::SimParticle> field
// "sim_particles". SimParticles are simulation *output*, so when reading them
// back as simulation *input* they are projected onto MCParticles.

#include <spdlog/spdlog.h>

#include <ROOT/RNTupleReader.hxx>
#include <ROOT/RNTupleView.hxx>
#include <SHiP/MCParticle.hpp>
#include <SHiP/SimParticle.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "math_utils.hpp"
#include "mc_particle_source.hpp"

namespace {

// Project a simulation SimParticle onto an MCParticle suitable as sim input.
// SimParticle::energy is kinetic while MCParticle::energy is total; recover the
// total energy from |p| and the kinetic energy without needing a PDG mass
// table: E_total = (|p|^2 + E_kin^2) / (2 E_kin), from
// m = (|p|^2 - E_kin^2) / (2 E_kin) and E_total = E_kin + m.
SHiP::MCParticle to_mc_particle(SHiP::SimParticle const& sp) {
  SHiP::MCParticle mc;
  mc.pdgCode = sp.pdgCode;
  mc.vertex = sp.vertex;
  mc.momentum = sp.momentum;
  double const p = aegir::magnitude(sp.momentum);
  double const e_kin = sp.energy;
  mc.energy = e_kin > 0.0 ? (p * p + e_kin * e_kin) / (2.0 * e_kin) : p;
  mc.time = sp.time;
  // SimParticle::parentId is the parent's track id (0 for primaries); map the
  // primary case to MCParticle's -1 convention.
  mc.motherId = sp.parentId == 0 ? -1 : sp.parentId;
  mc.status = 1;
  return mc;
}

class FileSource : public phlex::source {
 public:
  FileSource(std::string const& input_file, std::string const& ntuple,
             std::string const& product, long skip)
      : skip_{skip}, read_sim_{product == "sim_particles"} {
    // skip_ is cast to the unsigned ROOT::NTupleSize_t in generate(); reject a
    // negative skip here rather than let it wrap around to a huge offset.
    if (skip < 0)
      throw std::runtime_error(
          "file_source: 'skip' must be non-negative, got " +
          std::to_string(skip));

    if (product != "mc_particles" && product != "sim_particles")
      throw std::runtime_error(
          "file_source: unknown product '" + product +
          "' (expected 'mc_particles' or 'sim_particles')");

    reader_ = ROOT::RNTupleReader::Open(ntuple, input_file);
    n_entries_ = reader_->GetNEntries();
    if (read_sim_)
      sim_view_ = reader_->GetView<std::vector<SHiP::SimParticle>>(product);
    else
      mc_view_ = reader_->GetView<std::vector<SHiP::MCParticle>>(product);
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto const entry = static_cast<ROOT::NTupleSize_t>(id.number()) +
                       static_cast<ROOT::NTupleSize_t>(skip_);
    if (entry >= n_entries_) {
      spdlog::warn("file_source: entry {} out of range (file has {})", entry,
                   n_entries_);
      return {};
    }

    if (read_sim_) {
      auto const& sim_particles = (*sim_view_)(entry);
      std::vector<SHiP::MCParticle> particles;
      particles.reserve(sim_particles.size());
      for (auto const& sp : sim_particles)
        particles.push_back(to_mc_particle(sp));
      return particles;
    }
    return (*mc_view_)(entry);
  }

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    // A single RNTupleReader/view is not thread-safe; read events serially.
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::serial);
  }

  phlex::index_generator indices() override { co_return; }

 private:
  long skip_;
  bool read_sim_;
  ROOT::NTupleSize_t n_entries_{0};
  std::unique_ptr<ROOT::RNTupleReader> reader_;
  std::optional<ROOT::RNTupleView<std::vector<SHiP::MCParticle>>> mc_view_;
  std::optional<ROOT::RNTupleView<std::vector<SHiP::SimParticle>>> sim_view_;
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  using namespace phlex;

  auto input_file = config.get<std::string>("input_file");
  auto ntuple = config.get<std::string>("ntuple", std::string{"events"});
  auto product =
      config.get<std::string>("product", std::string{"mc_particles"});
  auto skip = config.get<long>("skip", 0L);  // start reading at this entry

  s.add_source<FileSource>("file_source", input_file, ntuple, product, skip);
}
