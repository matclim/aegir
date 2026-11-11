// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// nucleus_frame_test.cpp — the proton-nucleus set-up of fixed_target_source:
// the nucleus PDG code, the nucleon-nucleon centre-of-mass frame and its boost
// back to the lab, and the Pythia settings it writes (on a stand-in, so the
// test needs neither a Pythia initialisation nor its data files)

#include <SHiP/Units.hpp>
#include <algorithm>
#include <cmath>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pythia_common.hpp"

namespace {

struct Checker {
  int failures = 0;

  void operator()(bool ok, char const* what) {
    if (!ok) {
      std::println(stderr, "FAILED: {}", what);
      ++failures;
    }
  }
};

// Records the settings written; m0 as Pythia's proton mass.
struct StandInPythia {
  struct {
    double m0(int id) const { return id == 2212 ? 0.93827 : 0.; }
    // as Pythia's own table: 208Pb is known, 96Mo is not
    bool isParticle(int id) const { return id == 2212 || id == 1000822080; }
  } particleData;
  std::vector<std::string> lines;
  void readString(std::string const& s) { lines.push_back(s); }
  bool has(std::string const& s) const {
    return std::ranges::find(lines, s) != lines.end();
  }
};

// (E, pz) boosted along z by beta.
std::pair<double, double> boost(double e, double pz, double beta) {
  double const g = 1 / std::sqrt(1 - beta * beta);
  return {g * (e + beta * pz), g * (pz + beta * e)};
}

}  // namespace

int main() {
  namespace su = SHiP::units;
  using SHiP::aegir::nucleon_pair_frame;
  using SHiP::aegir::nucleus_pdg;
  Checker check;

  // ── nucleus codes ─────────────────────────────────────────────────────────
  check(nucleus_pdg(42, 96) == 1000420960, "96Mo is 1000420960");
  check(nucleus_pdg(74, 184) == 1000741840, "184W is 1000741840");
  check(nucleus_pdg(1, 1) == 1000010010, "a proton as a nucleus");
  for (auto [z, a] : std::vector<std::pair<int, int>>{{0, 1}, {43, 42}, {1, 1000}}) {
    bool threw = false;
    try {
      (void)nucleus_pdg(z, a);
    } catch (std::invalid_argument const&) {
      threw = true;
    }
    check(threw, "an invalid nucleus is rejected");
  }

  // ── centre-of-mass frame of 400 GeV p on a nucleon at rest ───────────────
  double const m = 0.93827;
  auto const f = nucleon_pair_frame(400.0 * su::GeV, m);
  check(std::abs(f.ecm - 27.4295) < 1e-3, "sqrt(s_NN) = 27.430 GeV at 400 GeV");
  check(f.beta > 0.997 && f.beta < 0.998, "beta of the NN frame");
  // The beams in the centre-of-mass frame, boosted by beta, are the lab beams:
  // the beam with 400 GeV along +z, the target nucleon at rest.
  double const estar = f.ecm / 2;
  double const pstar = std::sqrt(estar * estar - m * m);
  auto const [ea, pza] = boost(estar, pstar, f.beta);
  auto const [eb, pzb] = boost(estar, -pstar, f.beta);
  check(std::abs(ea - 400.0) < 1e-6, "beam energy 400 GeV in the lab");
  check(std::abs(pza - std::sqrt(400.0 * 400.0 - m * m)) < 1e-6,
        "beam momentum in the lab");
  check(std::abs(pzb) < 1e-9 && std::abs(eb - m) < 1e-9,
        "target nucleon at rest in the lab");
  for (double const e : {0.5, m}) {
    bool threw = false;
    try {
      (void)nucleon_pair_frame(e * su::GeV, m);
    } catch (std::invalid_argument const&) {
      threw = true;
    }
    check(threw, "a beam energy not above the nucleon mass is rejected");
  }

  // ── settings written for Angantyr ────────────────────────────────────────
  StandInPythia p;
  auto const g =
      SHiP::aegir::configure_nucleus_target(p, 42, 96, 400.0 * su::GeV);
  check(std::abs(g.ecm - f.ecm) < 1e-12 && std::abs(g.beta - f.beta) < 1e-12,
        "configure_nucleus_target returns the NN frame");
  check(p.has("Beams:idA = 2212"), "proton beam");
  check(p.has("Beams:idB = 1000420960"), "96Mo target");
  check(p.has("Beams:frameType = 1"), "generated in the CM frame");
  check(p.has("Beams:eCM = " + std::to_string(f.ecm)), "eCM = sqrt(s_NN)");
  check(p.has("HeavyIon:mode = 1"), "Angantyr for nuclear beams");
  check(std::ranges::count_if(p.lines, [](auto const& s) {
          return s.starts_with("1000420960:all = Z42A96 Z42A96bar 0 126 0 89.4");
        }) == 1,
        "96Mo, unknown to Pythia, defined with charge 3Z and mass A u");
  StandInPythia lead;
  (void)SHiP::aegir::configure_nucleus_target(lead, 82, 208, 400.0 * su::GeV);
  check(std::ranges::none_of(
            lead.lines, [](auto const& s) { return s.find(":all") != s.npos; }),
        "a nucleus Pythia knows (208Pb) is left as it is");
  check(std::ranges::none_of(
            p.lines, [](auto const& s) { return s.starts_with("Beams:eB"); }),
        "no fixed-target eB (frameType 2) setting");

  if (check.failures != 0) {
    std::println(stderr, "{} check(s) failed", check.failures);
    return 1;
  }
  std::println("nucleus_frame_test: all checks passed");
  return 0;
}
