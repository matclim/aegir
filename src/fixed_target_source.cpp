// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// fixed_target_source.cpp — Phlex source plugin for fixed-target collisions
//
// Dual-target Pythia8 source matching FairShip's FixedTargetGenerator:
// - Two Pythia instances (p-p and p-n) selected per-event by Z/A ratio
// - Interaction point sampled from truncated exponential in target material
// - Long-lived particles made stable for G4 decay
// - Multiple physics processes matching FairShip defaults

#include <Pythia8/Pythia.h>
#include <spdlog/spdlog.h>

#include <SHiP/MCParticle.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "mc_particle_source.hpp"
#include "philox_rng.hpp"

namespace {

// Extract final-state particles from a Pythia event record, offsetting vertex z
std::vector<SHiP::MCParticle> extract_particles(Pythia8::Event const& event,
                                                double z_offset) {
  std::vector<SHiP::MCParticle> particles;
  particles.reserve(event.size());
  for (int i = 0; i < event.size(); ++i) {
    auto const& p = event[i];
    if (!p.isFinal()) continue;

    SHiP::MCParticle mc;
    mc.pdgCode = p.id();
    mc.vertex = {p.xProd(), p.yProd(), p.zProd() + z_offset};  // mm
    mc.momentum = {p.px(), p.py(), p.pz()};                    // GeV
    mc.energy = p.e();
    mc.time = p.tProd() / 299.792458;  // mm/c -> ns
    mc.motherId = p.mother1();
    mc.status = p.statusHepMC();
    particles.push_back(mc);
  }
  return particles;
}

// Make long-lived particles stable so G4 handles their decay
void stabilise_long_lived(Pythia8::Pythia& pythia, double tau0_threshold) {
  for (auto it = pythia.particleData.begin(); it != pythia.particleData.end();
       ++it) {
    auto& entry = *it->second;
    if (entry.tau0() > tau0_threshold) {
      entry.setMayDecay(false);
    }
  }
}

void configure_processes(Pythia8::Pythia& pythia) {
  pythia.readString("SoftQCD:inelastic = on");
  pythia.readString("PhotonCollision:gmgm2mumu = on");
  pythia.readString("PromptPhoton:all = on");
  pythia.readString("WeakBosonExchange:all = on");
  pythia.readString("WeakSingleBoson:all = on");
}

class FixedTargetSource : public phlex::source {
 public:
  FixedTargetSource(std::string const& xml_dir, double beam_energy,
                    int target_z, int target_a, double target_z_start,
                    double target_z_end, double interaction_length,
                    double tau0_threshold)
      : target_z_{target_z},
        target_a_{target_a},
        target_z_start_{target_z_start},
        target_z_end_{target_z_end},
        interaction_length_{interaction_length} {
    auto beam_e_str = std::to_string(beam_energy);

    // Proton target (p-p)
    pythia_pp_ = std::make_unique<Pythia8::Pythia>(xml_dir, false);
    pythia_pp_->readString("Beams:idA = 2212");
    pythia_pp_->readString("Beams:idB = 2212");
    pythia_pp_->readString("Beams:frameType = 2");
    pythia_pp_->readString("Beams:eA = " + beam_e_str);
    pythia_pp_->readString("Beams:eB = 0.");
    configure_processes(*pythia_pp_);
    pythia_pp_->readString("Print:quiet = on");
    stabilise_long_lived(*pythia_pp_, tau0_threshold);
    pythia_pp_->init();

    // Neutron target (p-n)
    pythia_pn_ = std::make_unique<Pythia8::Pythia>(xml_dir, false);
    pythia_pn_->readString("Beams:idA = 2212");
    pythia_pn_->readString("Beams:idB = 2112");
    pythia_pn_->readString("Beams:frameType = 2");
    pythia_pn_->readString("Beams:eA = " + beam_e_str);
    pythia_pn_->readString("Beams:eB = 0.");
    configure_processes(*pythia_pn_);
    pythia_pn_->readString("Print:quiet = on");
    stabilise_long_lived(*pythia_pn_, tau0_threshold);
    pythia_pn_->init();
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto event_number = static_cast<std::uint32_t>(id.number());
    // 0xF14ED0A7: independent stream from the particle gun (0xBEEFCAFE default).
    aegir::PhiloxRng rng{event_number, 0xF14ED0A7};

    // Select target: proton with probability Z/A, else neutron
    double z_over_a =
        static_cast<double>(target_z_) / static_cast<double>(target_a_);
    bool proton_target = rng.uniform() < z_over_a;

    // Sample interaction point z from truncated exponential
    double target_length = target_z_end_ - target_z_start_;
    double u = rng.uniform();
    double exp_ratio = std::exp(-target_length / interaction_length_);
    double z_interaction =
        target_z_start_ -
        interaction_length_ * std::log(1.0 - u * (1.0 - exp_ratio));

    auto& pythia = proton_target ? *pythia_pp_ : *pythia_pn_;
    if (!pythia.next()) {
      spdlog::warn("FixedTargetSource: Pythia8 event generation failed");
      return {};
    }

    return extract_particles(pythia.event, z_interaction);
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
  int target_z_;
  int target_a_;
  double target_z_start_;
  double target_z_end_;
  double interaction_length_;
  std::unique_ptr<Pythia8::Pythia> pythia_pp_;
  std::unique_ptr<Pythia8::Pythia> pythia_pn_;
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  using namespace phlex;

  auto xml_dir = config.get<std::string>("xml_dir", [] {
    if (auto const* env = std::getenv("PYTHIA8DATA")) return std::string{env};
    return std::string{"../share/Pythia8/xmldoc"};
  }());
  auto beam_energy = config.get<double>("beam_energy", 400.0);
  auto target_z = config.get<int>("target_z", 74);
  auto target_a = config.get<int>("target_a", 184);
  auto target_z_start = config.get<double>("target_z_start", 0.0);
  auto target_z_end = config.get<double>("target_z_end", 1164.0);
  auto interaction_length = config.get<double>("interaction_length", 191.9);
  auto tau0_threshold = config.get<double>("tau0_threshold", 1.0);

  s.add_source<FixedTargetSource>(
      "fixed_target", xml_dir, beam_energy, target_z, target_a, target_z_start,
      target_z_end, interaction_length, tau0_threshold);
}
