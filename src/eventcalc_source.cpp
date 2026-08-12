// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// eventcalc_source.cpp — Phlex source plugin reading EventCalc-SHiP records
//
// Provides MCParticle vectors from an EventCalc `<LLP>_..._data.dat` file: a
// plain-text record of sampled LLP decays inside the SHiP decay volume, with
// the LLP kinematics, the decay vertex and the metastable decay products.
// Reading it needs no EventCalc installation, so LLP phenomenology stays
// decoupled from the aegir build; see docs/eventcalc.md for how to produce
// the input files.
//
// The LLP itself is not emitted by default: Geant4 has no particle definition
// for an HNL or a dark scalar, and the decay has already happened upstream.
// What is tracked are the daughters, all originating at the decay vertex.
//
// EventCalc rows are not equally probable — each carries the probability that
// its LLP decays inside the volume — so the source also publishes an
// EventHeader whose weight is that probability. Any distribution built from
// this sample must be weighted by it.

#include <SHiP/EventHeader.hpp>
#include <SHiP/MCParticle.hpp>
#include <SHiP/Units.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "eventcalc_reader.hpp"
#include "mc_particle_source.hpp"
#include "units/config_units.hpp"

namespace {

namespace su = ship::units;

bool is_neutrino(std::int32_t pdg) {
  auto const a = std::abs(pdg);
  return a == 12 || a == 14 || a == 16;
}

class EventCalcSource : public phlex::source {
 public:
  EventCalcSource(std::string const& file, long long first_event,
                  bool skip_neutrinos, bool emit_llp, ship::Length offset_x,
                  ship::Length offset_y, ship::Length offset_z)
      : reader_{file},
        first_event_{first_event},
        skip_neutrinos_{skip_neutrinos},
        emit_llp_{emit_llp},
        offset_{offset_x.numerical_value_in(su::mm),
                offset_y.numerical_value_in(su::mm),
                offset_z.numerical_value_in(su::mm)} {}

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto const& event = reader_.at(entry_for(id));

    // EventCalc writes metres with the origin at the centre of the SHiP
    // target; the values acquire their unit here and convert to the canonical
    // storage unit. The offset shifts into the frame of the configured
    // geometry provider, should the two origins differ.
    std::array<double, 3> const vertex{
        (event.vertex[0] * su::m).numerical_value_in(su::mm) + offset_[0],
        (event.vertex[1] * su::m).numerical_value_in(su::mm) + offset_[1],
        (event.vertex[2] * su::m).numerical_value_in(su::mm) + offset_[2]};

    double const time = flight_time(vertex, event.llp);

    std::vector<SHiP::MCParticle> particles;
    particles.reserve(event.daughters.size() + (emit_llp_ ? 1 : 0));

    std::int32_t mother = -1;
    if (emit_llp_) {
      SHiP::MCParticle llp;
      llp.pdgCode = event.llp.pdg;
      llp.momentum = event.llp.momentum;  // GeV
      llp.energy = event.llp.energy;      // GeV
      llp.vertex = offset_;               // produced at the target
      llp.time = 0.0;
      llp.motherId = -1;
      llp.status = 2;  // decayed: not a Geant4 primary
      particles.push_back(llp);
      mother = 0;
    }

    for (auto const& d : event.daughters) {
      if (skip_neutrinos_ && is_neutrino(d.pdg)) continue;
      SHiP::MCParticle mc;
      mc.pdgCode = d.pdg;
      mc.momentum = d.momentum;  // GeV
      mc.energy = d.energy;      // GeV
      mc.vertex = vertex;
      mc.time = time;
      mc.motherId = mother;
      mc.status = 1;
      particles.push_back(mc);
    }
    return particles;
  }

  // The weight is P_decay,LLP — the probability that this LLP decays inside
  // the volume — and it varies row to row, so it belongs on the event rather
  // than on each particle. original_event_id records which row of the input
  // this event came from, so a simulated event can be traced back to the
  // EventCalc record even after splitting a file across jobs.
  SHiP::EventHeader header(phlex::data_cell_index const& id) {
    auto const entry = entry_for(id);
    return SHiP::EventHeader{reader_.at(entry).decay_probability,
                             static_cast<std::int64_t>(entry)};
  }

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    // The reader is immutable after construction, so unlike the GENIE source
    // (whose TTree forces serial access) events can be served concurrently.
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::unlimited,
        [this](phlex::data_cell_index const& id) { return header(id); });
  }

  phlex::index_generator indices() override { co_return; }

 private:
  std::size_t entry_for(phlex::data_cell_index const& id) const {
    auto const entry = first_event_ + static_cast<long long>(id.number());
    if (entry < 0 || static_cast<std::size_t>(entry) >= reader_.size())
      throw std::runtime_error(
          "eventcalc_source: input exhausted — the workflow requested event " +
          std::to_string(entry) + " but '" + reader_.file_name() +
          "' holds only " + std::to_string(reader_.size()) +
          " decays. Reduce the driver's event count (see "
          "scripts/count_eventcalc_events.py) or provide a larger file.");
    return static_cast<std::size_t>(entry);
  }

  // EventCalc records no time, so it is reconstructed from the LLP kinematics:
  // the particle leaves the target at t = 0 and reaches the decay vertex after
  // a path length s at speed beta. Expressing s/beta in mm/c yields the time
  // directly, the same route Pythia's tProd() takes. Neglecting the flight of
  // the parent meson is a sub-ns approximation.
  static double flight_time(std::array<double, 3> const& vertex,
                            aegir::eventcalc::Particle const& llp) {
    double const p =
        std::hypot(llp.momentum[0], llp.momentum[1], llp.momentum[2]);
    if (p <= 0.0 || llp.energy <= 0.0) return 0.0;
    double const beta = p / llp.energy;
    double const path = std::hypot(vertex[0], vertex[1], vertex[2]);
    return ((path / beta) * su::mm_per_c).numerical_value_in(su::ns);
  }

  aegir::eventcalc::Reader reader_;
  long long first_event_;
  bool skip_neutrinos_;
  bool emit_llp_;
  std::array<double, 3> offset_;  // mm
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  auto file = config.get<std::string>("file");
  auto first_event = config.get<long>("first_event", 0L);
  if (first_event < 0)
    throw std::runtime_error("eventcalc_source: first_event must be >= 0");

  // Neutrinos are recorded by EventCalc but invisible to the detector;
  // tracking them costs time and yields nothing.
  auto skip_neutrinos = config.get<bool>("skip_neutrinos", true);
  // Only meaningful with a custom G4ParticleDefinition for the LLP.
  auto emit_llp = config.get<bool>("emit_llp", false);

  auto offset_x = aegir::get_quantity(config, "offset_x", 0.0 * su::mm);
  auto offset_y = aegir::get_quantity(config, "offset_y", 0.0 * su::mm);
  auto offset_z = aegir::get_quantity(config, "offset_z", 0.0 * su::mm);

  s.add_source<EventCalcSource>("eventcalc", file,
                                static_cast<long long>(first_event),
                                skip_neutrinos, emit_llp, offset_x, offset_y,
                                offset_z);
}
