// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geant4_tuning_test.cpp — Geant4 physics parameters from the configuration
//
// Reads parameter blocks, applies them to Geant4's singletons in PreInit (the
// state a fresh process is in, on its master thread) and checks that Geant4
// holds the values, and that unknown names, rejected values and failing
// commands are errors. No run manager or physics list is needed.

#include "geant4_tuning.hpp"

#include <G4HadronicDeveloperParameters.hh>
#include <G4HadronicParameters.hh>
#include <G4StateManager.hh>
#include <boost/json.hpp>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>

#include "phlex/configuration.hpp"
#include "units/clhep_bridge.hpp"

namespace {

struct Checker {
  int failures = 0;

  void operator()(bool ok, std::string const& what) {
    if (!ok) {
      std::println(stderr, "FAILED: {}", what);
      ++failures;
    }
  }
};

phlex::configuration config(std::string_view json) {
  return phlex::configuration{boost::json::parse(json).as_object()};
}

// True if f throws std::invalid_argument whose message contains `text`.
template <typename F>
bool throws(F&& f, std::string_view text) {
  try {
    f();
  } catch (std::invalid_argument const& e) {
    return std::string_view{e.what()}.find(text) != std::string_view::npos;
  }
  return false;
}

}  // namespace

int main() {
  using SHiP::aegir::apply_geant4_commands;
  using SHiP::aegir::apply_hadronic_parameters;
  using SHiP::aegir::read_geant4_tuning;
  Checker check;

  // ── reading ──────────────────────────────────────────────────────────────
  check(read_geant4_tuning(config("{}")).empty(), "no keys: nothing to apply");
  check(throws(
            [] {
              read_geant4_tuning(config(
                  R"({"hadronic_parameters": {"xs_factor_pion": 1.1}})"));
            },
            "unknown hadronic_parameters entry 'xs_factor_pion'"),
        "unknown hadronic parameter rejected");
  check(throws(
            [] {
              read_geant4_tuning(
                  config(R"({"hadronic_flags": {"apply_xs": true}})"));
            },
            "unknown hadronic_flags entry 'apply_xs'"),
        "unknown hadronic flag rejected");
  check(
      throws(
          [] {
            read_geant4_tuning(config(
                R"({"hadronic_parameters": {"xs_factor_pion_inelastic": 1.1}})"));
          },
          "apply_factor_xs"),
      "cross-section factor without apply_factor_xs rejected");

  // ── applying ─────────────────────────────────────────────────────────────
  auto const good = read_geant4_tuning(config(R"({
    "hadronic_flags": {"apply_factor_xs": true, "enable_cr_coalescence": true},
    "hadronic_parameters": {
      "xs_factor_pion_inelastic": 1.1,
      "xs_factor_nucleon_inelastic": 0.95,
      "min_energy_transition_ftf_cascade": 4.0,
      "max_energy_transition_ftf_cascade": 6.0
    },
    "developer_parameters": {"FTF_PION_AVRG_PT2": 0.25},
    "developer_flags": {"FTF_PION_DIFF_DISSO_PROJ": true},
    "commands": ["/process/had/models/ftf/selectTuneByName pion-tune2022-v0"]
  })"));
  try {
    apply_hadronic_parameters(good);
    apply_geant4_commands(good);
  } catch (std::exception const& e) {
    check(false, std::string{"valid parameters applied: "} + e.what());
  }
  auto const& hp = *G4HadronicParameters::Instance();
  check(hp.ApplyFactorXS(), "apply_factor_xs set");
  check(hp.EnableCRCoalescence(), "enable_cr_coalescence set");
  check(hp.XSFactorPionInelastic() == 1.1, "xs_factor_pion_inelastic = 1.1");
  check(hp.XSFactorNucleonInelastic() == 0.95,
        "xs_factor_nucleon_inelastic = 0.95");
  auto const gev = [](double e) {
    return SHiP::aegir::clhep::g4(SHiP::Energy{e * SHiP::units::GeV});
  };
  check(hp.GetMinEnergyTransitionFTF_Cascade() == gev(4.0),
        "FTF-cascade transition from 4 GeV");
  check(hp.GetMaxEnergyTransitionFTF_Cascade() == gev(6.0),
        "FTF-cascade transition to 6 GeV");
  auto& hdp = G4HadronicDeveloperParameters::GetInstance();
  double pt2 = 0;
  bool diff = false;
  check(hdp.Get("FTF_PION_AVRG_PT2", pt2) && pt2 == 0.25,
        "developer parameter FTF_PION_AVRG_PT2 = 0.25");
  check(hdp.Get("FTF_PION_DIFF_DISSO_PROJ", diff) && diff,
        "developer flag FTF_PION_DIFF_DISSO_PROJ set");
  check(G4FTFTunings::Instance()->GetTuneApplicabilityState(2) == 1,
        "FTF tune pion-tune2022-v0 selected");

  // Values Geant4 does not take are errors, not silently ignored.
  check(throws(
            [] {
              apply_hadronic_parameters(read_geant4_tuning(config(R"({
          "hadronic_flags": {"apply_factor_xs": true},
          "hadronic_parameters": {"xs_factor_pion_elastic": 1.5}})")));
            },
            "|factor - 1| < 0.2"),
        "cross-section factor beyond Geant4's limit rejected");
  check(throws(
            [] {
              apply_hadronic_parameters(read_geant4_tuning(config(R"({
          "developer_parameters": {"FTF_NO_SUCH_PARAMETER": 1.0}})")));
            },
            "does not exist"),
        "unknown developer parameter rejected, not aborted");
  check(throws(
            [] {
              apply_hadronic_parameters(read_geant4_tuning(config(R"({
          "developer_parameters": {"FTF_BARYON_AVRG_PT2": 100.0}})")));
            },
            "outside the allowable range"),
        "developer parameter out of range rejected, not aborted");
  check(throws(
            [] {
              apply_hadronic_parameters(read_geant4_tuning(config(R"({
          "developer_parameters": {"FTF_PION_AVRG_PT2": 0.2}})")));
            },
            "already been changed"),
        "developer parameter changed twice rejected, not aborted");
  check(throws(
            [] {
              apply_geant4_commands(read_geant4_tuning(config(R"({
          "commands": ["/no/such/command 1"]})")));
            },
            "command not found"),
        "unknown command rejected");
  check(throws(
            [] {
              apply_geant4_commands(read_geant4_tuning(config(R"({
          "commands": ["/process/had/models/ftf/selectTuneByIndex 99"]})")));
            },
            "/process/had/models/ftf/selectTuneByIndex 99"),
        "command whose messenger reports a failure rejected");
  check(throws(
            [] {
              apply_geant4_commands(read_geant4_tuning(config(R"({
          "commands": ["/process/had/models/ftf/selectTuneByIndex -1"]})")));
            },
            "parameter out of range"),
        "command with a parameter out of range rejected");
  check(G4StateManager::GetStateManager()->GetExceptionHandler() == nullptr,
        "exception handler restored (there was none before)");

  if (check.failures == 0) {
    std::println("geant4_tuning_test: all checks passed");
  }
  return check.failures == 0 ? 0 : 1;
}
