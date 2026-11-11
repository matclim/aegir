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
// - Every random number an event uses is keyed to its event number, so
//   the output does not depend on the order events are processed in
// - Any Pythia8 setting can be added with `pythia_settings` / `pythia_cmnd`,
//   applied to both instances (see pythia_settings.hpp)
// - nucleus: true replaces the p-p/p-n pair by one proton-nucleus collision
//   with Pythia's heavy-ion model Angantyr, generated in the nucleon-nucleon
//   centre-of-mass frame and boosted to the lab. Its internal generators draw
//   from Pythia's own random engine (Random:seed from the job seed), so these
//   events are reproducible for a given seed in the order the (serial) source
//   generates them, but are not keyed to the event number.

#include <Pythia8/Pythia.h>
#include <spdlog/spdlog.h>

#include <SHiP/MCParticle.hpp>
#include <SHiP/random/philox_rng.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mc_particle_source.hpp"
#include "pythia_common.hpp"
#include "pythia_philox_engine.hpp"
#include "pythia_settings.hpp"
#include "seed_config.hpp"
#include "units/config_units.hpp"

namespace {

namespace su = SHiP::units;

void configure_processes(Pythia8::Pythia& pythia) {
  pythia.readString("SoftQCD:inelastic = on");
  pythia.readString("PhotonCollision:gmgm2mumu = on");
  pythia.readString("PromptPhoton:all = on");
  pythia.readString("WeakBosonExchange:all = on");
  pythia.readString("WeakSingleBoson:all = on");
}

class FixedTargetSource : public phlex::source {
 public:
  FixedTargetSource(std::string stage, std::string const& xml_dir,
                    SHiP::Energy beam_energy, int target_z, int target_a,
                    SHiP::Length target_z_start, SHiP::Length target_z_end,
                    SHiP::Length interaction_length,
                    SHiP::aegir::PythiaTime tau0_threshold,
                    SHiP::aegir::PythiaSettings const& settings,
                    SHiP::aegir::record_mode record, std::uint32_t seed,
                    bool nucleus)
      : stage_{std::move(stage)},
        target_z_{target_z},
        target_a_{target_a},
        target_z_start_{target_z_start},
        target_z_end_{target_z_end},
        interaction_length_{interaction_length},
        record_{record},
        seed_{seed},
        nucleus_{nucleus} {
    // Both instances draw from Philox engines that generate() re-keys to the
    // event number before every event (see pythia_philox_engine.hpp). Pythia
    // also draws while initialising; distinct init sub-streams keep the two
    // instances apart there.
    auto const attach_engine =
        [seed](Pythia8::Pythia& pythia,
               std::shared_ptr<SHiP::aegir::PhiloxRndmEngine> const& engine,
               std::uint32_t init_substream) {
          engine->reset(seed, SHiP::aegir::kPythiaInitStream, init_substream);
          pythia.setRndmEnginePtr(engine);
        };

    if (nucleus_) {
      // One proton-nucleus collision per event (Angantyr), in pythia_pp_.
      pythia_pp_ = std::make_unique<Pythia8::Pythia>(xml_dir, false);
      attach_engine(*pythia_pp_, engine_pp_, 0);
      auto const frame = SHiP::aegir::configure_nucleus_target(
          *pythia_pp_, target_z, target_a, beam_energy);
      boost_beta_ = frame.beta;
      // Minimum bias only: the photon and electroweak processes of the p-N
      // set-up are not part of Angantyr's sub-collision model.
      pythia_pp_->readString("SoftQCD:inelastic = on");
      // Angantyr's sub-generators use Pythia's own engine, not the one
      // attached above: seed it from the job seed.
      pythia_pp_->readString("Random:setSeed = on");
      pythia_pp_->readString("Random:seed = " +
                             std::to_string(SHiP::aegir::pythia_seed(seed)));
      pythia_pp_->readString("Print:quiet = on");
      SHiP::aegir::stabilise_long_lived(*pythia_pp_, tau0_threshold);
      SHiP::aegir::apply_pythia_settings(*pythia_pp_, settings,
                                         "fixed_target (p-A)");
      if (!pythia_pp_->init()) {
        throw std::runtime_error(
            "fixed_target: Pythia8 (p-A, Angantyr) initialisation failed");
      }
      spdlog::info(
          "fixed_target: p + nucleus (Z = {}, A = {}) with Angantyr, "
          "sqrt(s_NN) = {:.3f} GeV, boost to the lab beta = {:.6f}",
          target_z, target_a, frame.ecm, frame.beta);
      spdlog::info(
          "fixed_target: Pythia8 settings changed from the defaults (p-A):\n{}",
          SHiP::aegir::changed_pythia_settings(*pythia_pp_));
      return;
    }

    // Proton target (p-p)
    pythia_pp_ = std::make_unique<Pythia8::Pythia>(xml_dir, false);
    attach_engine(*pythia_pp_, engine_pp_, 0);
    SHiP::aegir::configure_beams(*pythia_pp_, 2212, 2212, beam_energy);
    configure_processes(*pythia_pp_);
    pythia_pp_->readString("Print:quiet = on");
    SHiP::aegir::stabilise_long_lived(*pythia_pp_, tau0_threshold);
    // After stabilise_long_lived, so particle-data settings can override it.
    SHiP::aegir::apply_pythia_settings(*pythia_pp_, settings,
                                       "fixed_target (pp)");
    if (!pythia_pp_->init()) {
      throw std::runtime_error(
          "fixed_target: Pythia8 (pp) initialisation failed");
    }

    // Neutron target (p-n)
    pythia_pn_ = std::make_unique<Pythia8::Pythia>(xml_dir, false);
    attach_engine(*pythia_pn_, engine_pn_, 1);
    SHiP::aegir::configure_beams(*pythia_pn_, 2212, 2112, beam_energy);
    configure_processes(*pythia_pn_);
    pythia_pn_->readString("Print:quiet = on");
    SHiP::aegir::stabilise_long_lived(*pythia_pn_, tau0_threshold);
    SHiP::aegir::apply_pythia_settings(*pythia_pn_, settings,
                                       "fixed_target (pn)");
    if (!pythia_pn_->init()) {
      throw std::runtime_error(
          "fixed_target: Pythia8 (pn) initialisation failed");
    }
    // The two instances differ only in Beams:idB.
    spdlog::info(
        "fixed_target: Pythia8 settings changed from the defaults (pp):\n{}",
        SHiP::aegir::changed_pythia_settings(*pythia_pp_));
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto event_number = static_cast<std::uint32_t>(id.number());
    // 0xF14ED0A7: independent stream from the particle gun (0xBEEFCAFE
    // default).
    SHiP::random::PhiloxRng rng{seed_, 0xF14ED0A7, event_number};

    if (nucleus_) {
      // Same interaction-point sampling as below (the rng's first draw is
      // the target choice there, unused here).
      (void)rng.uniform();
      SHiP::Length const z_interaction = sample_interaction_z(rng);
      engine_pp_->reset(seed_, SHiP::aegir::kPythiaEventStream, event_number);
      SHiP::aegir::next_event(*pythia_pp_, "FixedTargetSource (p-A)");
      // centre-of-mass frame -> lab (target nucleus at rest); vertices too
      pythia_pp_->event.bst(0., 0., boost_beta_);
      auto particles = SHiP::aegir::extract_particles<SHiP::MCParticle>(
          pythia_pp_->event, z_interaction, record_);
      // Nuclear remnants (spectators of the target, at rest) are not
      // tracked: Geant4 has no particle for them by PDG code.
      std::erase_if(particles, [](SHiP::MCParticle const& p) {
        return p.status == 1 && std::abs(p.pdg_code) >= 1000000000;
      });
      return particles;
    }

    // Select target: proton with probability Z/A, else neutron
    double const z_over_a =
        static_cast<double>(target_z_) / static_cast<double>(target_a_);
    bool const proton_target = rng.uniform() < z_over_a;

    SHiP::Length const z_interaction = sample_interaction_z(rng);

    auto& pythia = proton_target ? *pythia_pp_ : *pythia_pn_;
    // Key Pythia's draws to this event, whatever order events arrive in.
    (proton_target ? engine_pp_ : engine_pn_)
        ->reset(seed_, SHiP::aegir::kPythiaEventStream, event_number);
    SHiP::aegir::next_event(pythia, proton_target ? "FixedTargetSource (pp)"
                                                  : "FixedTargetSource (pn)");

    return SHiP::aegir::extract_particles<SHiP::MCParticle>(
        pythia.event, z_interaction, record_);
  }

  phlex::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    return SHiP::aegir::mc_particle_provider_bundles(
        selector, stage_,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::serial);
  }

 private:
  // Interaction point z from the truncated exponential in the target.
  SHiP::Length sample_interaction_z(SHiP::random::PhiloxRng& rng) const {
    SHiP::Length const target_length = target_z_end_ - target_z_start_;
    double const u = rng.uniform();
    double const exp_ratio = std::exp(-(target_length / interaction_length_)
                                           .numerical_value_in(mp_units::one));
    return target_z_start_ -
           interaction_length_ * std::log(1.0 - u * (1.0 - exp_ratio));
  }

  std::string stage_;
  int target_z_;
  int target_a_;
  SHiP::Length target_z_start_;
  SHiP::Length target_z_end_;
  SHiP::Length interaction_length_;
  std::shared_ptr<SHiP::aegir::PhiloxRndmEngine> engine_pp_ =
      std::make_shared<SHiP::aegir::PhiloxRndmEngine>();
  std::shared_ptr<SHiP::aegir::PhiloxRndmEngine> engine_pn_ =
      std::make_shared<SHiP::aegir::PhiloxRndmEngine>();
  std::unique_ptr<Pythia8::Pythia> pythia_pp_;
  std::unique_ptr<Pythia8::Pythia> pythia_pn_;
  SHiP::aegir::record_mode record_;
  std::uint32_t seed_;
  bool nucleus_;
  double boost_beta_ = 0.;
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  using namespace phlex;

  auto const xml_dir = config.get<std::string>("xml_dir", [] {
    if (auto const* env = std::getenv("PYTHIA8DATA")) {
      return std::string{env};
    }
    return std::string{"../share/Pythia8/xmldoc"};
  }());
  auto const beam_energy =
      SHiP::aegir::get_quantity(config, "beam_energy", 400.0 * su::GeV);
  auto const target_z = config.get<int>("target_z", 74);
  auto const target_a = config.get<int>("target_a", 184);
  auto const target_z_start =
      SHiP::aegir::get_quantity(config, "target_z_start", 0.0 * su::mm);
  auto const target_z_end =
      SHiP::aegir::get_quantity(config, "target_z_end", 1164.0 * su::mm);
  auto const interaction_length =
      SHiP::aegir::get_quantity(config, "interaction_length", 191.9 * su::mm);
  auto const tau0_threshold =
      SHiP::aegir::get_quantity(config, "tau0_threshold", 1.0 * su::mm_per_c);
  auto const record = SHiP::aegir::parse_record_mode(
      config.get<std::string>("record", std::string{"final_state"}),
      "fixed_target");
  SHiP::aegir::PythiaSettings const settings{
      .lines = config.get<std::vector<std::string>>("pythia_settings",
                                                    std::vector<std::string>{}),
      .cmnd_file = config.get<std::string>("pythia_cmnd", std::string{}),
  };
  auto const nucleus = config.get<bool>("nucleus", false);
  auto seed = SHiP::aegir::resolve_seed(config, "fixed_target");
  auto stage = SHiP::aegir::source_stage(config, "fixed_target");

  s.add_source<FixedTargetSource>(
      "fixed_target", std::move(stage), xml_dir, beam_energy, target_z,
      target_a, target_z_start, target_z_end, interaction_length,
      tau0_threshold, settings, record, seed, nucleus);
}
