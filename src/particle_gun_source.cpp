// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// particle_gun_source.cpp — Phlex source plugin
//
// Provides MCParticle vectors from a configurable particle gun.
// Each event generates a single particle with fixed or randomised kinematics.

#include <SHiP/MCParticle.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "mc_particle_source.hpp"
#include "philox_rng.hpp"

namespace {

class ParticleGun : public phlex::source {
 public:
  ParticleGun(int pdg, double p_min, double p_max, double max_theta,
              std::array<double, 3> vertex)
      : pdg_{pdg},
        p_min_{p_min},
        p_max_{p_max},
        max_theta_{max_theta},
        vertex_{vertex} {}

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto event_number = static_cast<std::uint32_t>(id.number());
    aegir::PhiloxRng rng{event_number};

    double p = rng.uniform(p_min_, p_max_);
    double theta = rng.uniform(0.0, max_theta_);
    double phi = rng.uniform(0.0, 2.0 * M_PI);

    SHiP::MCParticle mc;
    mc.pdgCode = pdg_;
    mc.vertex = vertex_;
    mc.momentum = {p * std::sin(theta) * std::cos(phi),
                   p * std::sin(theta) * std::sin(phi), p * std::cos(theta)};
    // Assume massless for energy (good enough for muons at high p)
    mc.energy = p;
    mc.time = 0.0;
    mc.motherId = -1;
    mc.status = 1;

    return {mc};
  }

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::unlimited);
  }

  phlex::index_generator indices() override { co_return; }

 private:
  int pdg_;
  double p_min_, p_max_, max_theta_;
  std::array<double, 3> vertex_;
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  using namespace phlex;

  auto pdg = config.get<int>("pdg", 13);           // muon
  auto p_min = config.get<double>("p_min", 10.0);  // GeV
  auto p_max = config.get<double>("p_max", 100.0);
  auto max_theta = config.get<double>("max_theta", 0.1);  // rad
  auto vx = config.get<double>("vertex_x", 0.0);
  auto vy = config.get<double>("vertex_y", 0.0);
  auto vz = config.get<double>("vertex_z", -500.0);  // mm, upstream of target

  s.add_source<ParticleGun>("particle_gun", pdg, p_min, p_max, max_theta,
                            std::array<double, 3>{vx, vy, vz});
}
