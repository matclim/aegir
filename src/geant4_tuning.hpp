// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geant4_tuning.hpp — Geant4 physics parameters from the job configuration
//
// Five geant4_module keys give access to the Geant4 physics parameters
// without a code change per parameter (docs/physics_parameters.md):
//
//   hadronic_parameters  {name: number}  G4HadronicParameters, by the names in
//                                        kHadronicNumbers below (energies in
//                                        GeV)
//   hadronic_flags       {name: bool}    G4HadronicParameters switches
//                                        (kHadronicFlags below)
//   developer_parameters {name: number}  G4HadronicDeveloperParameters: the FTF
//                                        model parameters (e.g.
//                                        FTF_PION_AVRG_PT2), in the units
//                                        Geant4 registers them in
//                                        (G4FTFTunings.cc; some GeV², some
//                                        internal)
//   developer_flags      {name: bool}    their switches (e.g.
//                                        FTF_PION_DIFF_DISSO_PROJ)
//   commands             [string]        Geant4 UI commands, e.g.
//                                        '/physics_lists/em/GammaToMuons true',
//                                        '/process/had/models/ftf/selectTuneByName
//                                        pion-tune2022-v0'
//
// All of them are applied on the master, in the PreInit state: the parameters
// before the physics list is created (its constructors read the transition
// energies), the commands after it is created (its messengers then exist) and
// before the run manager initialises. That is where Geant4 accepts them;
// G4HadronicParameters and the FTF tunes ignore changes in any other state.
// Workers build their physics from the same objects afterwards.
//
// Geant4 ignores a value it does not accept (out of range, locked) without an
// error, so every parameter is read back and a mismatch is an error, as are an
// unknown name and a command that does not succeed. Cross-section factors only
// act with apply_factor_xs, so setting one without that flag is an error too;
// Geant4 limits them to |factor - 1| < 0.2. The developer parameters report an
// unknown name, a value out of range or a second change as a fatal
// G4Exception, which would abort the process; while they and the commands are
// applied, a handler turns fatal G4Exceptions into C++ exceptions, so the job
// fails cleanly like any other configuration error.

#pragma once

#include <G4FTFTunings.hh>
#include <G4HadronicDeveloperParameters.hh>
#include <G4HadronicParameters.hh>
#include <G4StateManager.hh>
#include <G4UIcommandStatus.hh>
#include <G4UImanager.hh>
#include <G4VExceptionHandler.hh>
#include <G4ios.hh>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "phlex/configuration.hpp"
#include "units/clhep_bridge.hpp"

namespace SHiP::aegir {

struct Geant4Tuning {
  std::map<std::string, double> hadronic_parameters;
  std::map<std::string, bool> hadronic_flags;
  std::map<std::string, double> developer_parameters;
  std::map<std::string, bool> developer_flags;
  std::vector<std::string> commands;

  [[nodiscard]] bool empty() const {
    return hadronic_parameters.empty() && hadronic_flags.empty() &&
           developer_parameters.empty() && developer_flags.empty() &&
           commands.empty();
  }
};

namespace detail {

using HP = G4HadronicParameters;

struct HadronicNumber {
  std::string_view name;
  bool energy;  // configured in GeV, passed to Geant4 in its internal unit
  void (HP::*set)(G4double);
  G4double (HP::*get)() const;
};

struct HadronicFlag {
  std::string_view name;
  void (HP::*set)(G4bool);
  G4bool (HP::*get)() const;
};

// Applied in this order: a transition's minimum before its maximum, since
// Geant4 accepts a maximum only above the current minimum.
inline constexpr HadronicNumber kHadronicNumbers[] = {
    {"max_energy", true, &HP::SetMaxEnergy, &HP::GetMaxEnergy},
    {"min_energy_transition_ftf_cascade", true,
     &HP::SetMinEnergyTransitionFTF_Cascade,
     &HP::GetMinEnergyTransitionFTF_Cascade},
    {"max_energy_transition_ftf_cascade", true,
     &HP::SetMaxEnergyTransitionFTF_Cascade,
     &HP::GetMaxEnergyTransitionFTF_Cascade},
    {"min_energy_transition_qgs_ftf", true, &HP::SetMinEnergyTransitionQGS_FTF,
     &HP::GetMinEnergyTransitionQGS_FTF},
    {"max_energy_transition_qgs_ftf", true, &HP::SetMaxEnergyTransitionQGS_FTF,
     &HP::GetMaxEnergyTransitionQGS_FTF},
    {"energy_threshold_for_heavy_hadrons", true,
     &HP::SetEnergyThresholdForHeavyHadrons,
     &HP::EnergyThresholdForHeavyHadrons},
    {"xs_factor_nucleon_inelastic", false, &HP::SetXSFactorNucleonInelastic,
     &HP::XSFactorNucleonInelastic},
    {"xs_factor_nucleon_elastic", false, &HP::SetXSFactorNucleonElastic,
     &HP::XSFactorNucleonElastic},
    {"xs_factor_pion_inelastic", false, &HP::SetXSFactorPionInelastic,
     &HP::XSFactorPionInelastic},
    {"xs_factor_pion_elastic", false, &HP::SetXSFactorPionElastic,
     &HP::XSFactorPionElastic},
    {"xs_factor_hadron_inelastic", false, &HP::SetXSFactorHadronInelastic,
     &HP::XSFactorHadronInelastic},
    {"xs_factor_hadron_elastic", false, &HP::SetXSFactorHadronElastic,
     &HP::XSFactorHadronElastic},
    {"xs_factor_em", false, &HP::SetXSFactorEM, &HP::XSFactorEM},
};

inline constexpr HadronicFlag kHadronicFlags[] = {
    {"apply_factor_xs", &HP::SetApplyFactorXS, &HP::ApplyFactorXS},
    {"enable_bc_particles", &HP::SetEnableBCParticles, &HP::EnableBCParticles},
    {"enable_hyper_nuclei", &HP::SetEnableHyperNuclei, &HP::EnableHyperNuclei},
    {"enable_cr_coalescence", &HP::SetEnableCRCoalescence,
     &HP::EnableCRCoalescence},
    {"enable_integral_inelastic_xs", &HP::SetEnableIntegralInelasticXS,
     &HP::EnableIntegralInelasticXS},
    {"enable_integral_elastic_xs", &HP::SetEnableIntegralElasticXS,
     &HP::EnableIntegralElasticXS},
    {"enable_diff_dissociation_for_b_greater_10",
     &HP::SetEnableDiffDissociationForBGreater10,
     &HP::EnableDiffDissociationForBGreater10},
    {"enable_coherent_charge_exchange", &HP::SetEnableCoherentChargeExchange,
     &HP::EnableCoherentChargeExchange},
};

[[noreturn]] inline void tuning_error(std::string const& what) {
  throw std::invalid_argument("geant4_module: " + what);
}

inline std::string command_status(G4int status) {
  if (status > 0 && status < 100) {
    return "the command reported a failure (code " + std::to_string(status) +
           "; Geant4's message above says why)";
  }
  switch (status / 100 * 100) {
    case fCommandNotFound:
      return "command not found";
    case fIllegalApplicationState:
      return "not allowed in this Geant4 state";
    case fParameterOutOfRange:
      return "parameter out of range";
    case fParameterUnreadable:
      return "parameter unreadable";
    case fParameterOutOfCandidates:
      return "parameter not among the candidates";
    case fAliasNotFound:
      return "alias not found";
    default:
      return "status " + std::to_string(status);
  }
}

// Turns fatal G4Exceptions into std::invalid_argument for its lifetime;
// warnings are printed as usual. Restores the previous handler (the run
// manager's) when it goes out of scope.
class ThrowingExceptionHandler : public G4VExceptionHandler {
 public:
  // G4VExceptionHandler's constructor registers the new handler, so the
  // previous one is fetched first.
  ThrowingExceptionHandler()
      : ThrowingExceptionHandler{
            G4StateManager::GetStateManager()->GetExceptionHandler()} {}
  ~ThrowingExceptionHandler() override {
    G4StateManager::GetStateManager()->SetExceptionHandler(previous_);
  }
  ThrowingExceptionHandler(ThrowingExceptionHandler const&) = delete;
  ThrowingExceptionHandler& operator=(ThrowingExceptionHandler const&) = delete;

  G4bool Notify(char const* origin, char const* code,
                G4ExceptionSeverity severity,
                char const* description) override {
    if (severity == JustWarning) {
      G4cerr << "Geant4 warning " << code << " (" << origin
             << "): " << description << G4endl;
      return false;
    }
    throw std::invalid_argument(std::string{"Geant4 "} + code + " (" + origin +
                                "): " + description);
  }

 private:
  explicit ThrowingExceptionHandler(G4VExceptionHandler* previous)
      : previous_{previous} {}
  G4VExceptionHandler* previous_;
};

}  // namespace detail

// Read the five keys; unknown parameter names are rejected here, before any
// Geant4 object exists.
inline Geant4Tuning read_geant4_tuning(phlex::configuration const& config) {
  Geant4Tuning t;
  t.hadronic_parameters = config.get<std::map<std::string, double>>(
      "hadronic_parameters", std::map<std::string, double>{});
  t.hadronic_flags = config.get<std::map<std::string, bool>>(
      "hadronic_flags", std::map<std::string, bool>{});
  t.commands = config.get<std::vector<std::string>>("commands",
                                                    std::vector<std::string>{});
  for (auto const& [name, value] : t.hadronic_parameters) {
    if (std::ranges::none_of(detail::kHadronicNumbers,
                             [&](auto const& p) { return p.name == name; })) {
      detail::tuning_error("unknown hadronic_parameters entry '" + name +
                           "' (see docs/physics_parameters.md)");
    }
  }
  for (auto const& [name, value] : t.hadronic_flags) {
    if (std::ranges::none_of(detail::kHadronicFlags,
                             [&](auto const& p) { return p.name == name; })) {
      detail::tuning_error("unknown hadronic_flags entry '" + name +
                           "' (see docs/physics_parameters.md)");
    }
  }
  t.developer_parameters = config.get<std::map<std::string, double>>(
      "developer_parameters", std::map<std::string, double>{});
  t.developer_flags = config.get<std::map<std::string, bool>>(
      "developer_flags", std::map<std::string, bool>{});
  bool const factors = std::ranges::any_of(
      t.hadronic_parameters,
      [](auto const& e) { return e.first.starts_with("xs_factor_"); });
  auto const apply = t.hadronic_flags.find("apply_factor_xs");
  if (factors && (apply == t.hadronic_flags.end() || !apply->second)) {
    detail::tuning_error(
        "cross-section factors act only with hadronic_flags.apply_factor_xs: "
        "true");
  }
  return t;
}

// Before the physics list is created, on the master in PreInit.
inline void apply_hadronic_parameters(Geant4Tuning const& t) {
  auto& hp = *G4HadronicParameters::Instance();
  for (auto const& p : detail::kHadronicFlags) {
    if (auto const it = t.hadronic_flags.find(std::string{p.name});
        it != t.hadronic_flags.end()) {
      (hp.*p.set)(it->second);
      if ((hp.*p.get)() != it->second) {
        detail::tuning_error("Geant4 did not accept hadronic_flags." +
                             it->first);
      }
    }
  }
  for (auto const& p : detail::kHadronicNumbers) {
    if (auto const it = t.hadronic_parameters.find(std::string{p.name});
        it != t.hadronic_parameters.end()) {
      double const value = p.energy ? SHiP::aegir::clhep::g4(SHiP::Energy{
                                          it->second * SHiP::units::GeV})
                                    : it->second;
      (hp.*p.set)(value);
      if ((hp.*p.get)() != value) {
        detail::tuning_error(
            "Geant4 did not accept hadronic_parameters." + it->first + " = " +
            std::to_string(it->second) +
            (it->first.starts_with("xs_factor_")
                 ? " (cross-section factors need |factor - 1| < 0.2)"
                 : " (out of range, or a maximum not above its minimum)"));
      }
    }
  }
  // The developer parameters reject an unknown name, a value out of range and
  // a second change with a fatal G4Exception, turned into an exception here.
  if (t.developer_parameters.empty() && t.developer_flags.empty()) {
    return;
  }
  auto& hdp = G4HadronicDeveloperParameters::GetInstance();
  detail::ThrowingExceptionHandler const handler;
  auto const set = [&](std::string const& key, std::string const& name,
                       auto value) {
    try {
      if (!hdp.Set(name, value)) {
        detail::tuning_error("Geant4 did not accept " + key + "." + name);
      }
    } catch (std::invalid_argument const& e) {
      detail::tuning_error(key + "." + name + ": " + e.what());
    }
  };
  for (auto const& [name, value] : t.developer_parameters) {
    set("developer_parameters", name, G4double{value});
  }
  for (auto const& [name, value] : t.developer_flags) {
    set("developer_flags", name, G4bool{value});
  }
}

// After the physics list is created and before the run manager initialises,
// on the master in PreInit.
inline void apply_geant4_commands(Geant4Tuning const& t) {
  if (t.commands.empty()) {
    return;
  }
  // The FTF-tune commands exist only once their singleton does.
  G4FTFTunings::Instance();
  auto* ui = G4UImanager::GetUIpointer();
  detail::ThrowingExceptionHandler const handler;
  for (auto const& command : t.commands) {
    G4int status = fCommandSucceeded;
    try {
      status = ui->ApplyCommand(command);
    } catch (std::invalid_argument const& e) {
      detail::tuning_error("Geant4 command '" + command + "': " + e.what());
    }
    if (status != fCommandSucceeded) {
      detail::tuning_error("Geant4 command '" + command +
                           "' failed: " + detail::command_status(status));
    }
  }
}

}  // namespace SHiP::aegir
