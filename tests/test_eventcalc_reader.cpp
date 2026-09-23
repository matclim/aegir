// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// test_eventcalc_reader.cpp — unit test for the EventCalc .dat parser
//
// The parser is standard-library only, so this links nothing: no Phlex, ROOT
// or Geant4. Run with the fixture as the single argument:
//   ./test_eventcalc_reader tests/data/eventcalc_sample.dat
//
// The fixture deliberately covers the cases that have bitten this parser:
// a summary line whose values are followed by full stops, process names
// containing the decay arrow, block headers with and without sample_points,
// CRLF line endings, channels of differing multiplicity, rows padded with one
// and with two `0. 0. 0. 0. 0. -999.` groups, an anti-proton daughter whose
// PDG code sits far below the padding sentinel, and a final row that ends
// mid-group — truncated on purpose, so do not "repair" it.

#include <cstddef>
#include <iostream>
#include <string>

#include "eventcalc_reader.hpp"

namespace {

int failures = 0;

template <typename A, typename B>
void check(std::string const& what, A const& got, B const& expected) {
  bool const ok = (got == expected);
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << what << " = " << got << ", expected " << expected
              << "\n";
  }
}

// Indexed access guarded by the daughter count: a regression that shortened
// the list would otherwise be undefined behaviour rather than a reported
// failure.
void check_daughter(std::string const& what, aegir::eventcalc::Reader const& r,
                    std::size_t event, std::size_t daughter,
                    std::int32_t expected) {
  auto const& daughters = r.at(event).daughters;
  if (daughter >= daughters.size()) {
    ++failures;
    std::cerr << "FAIL: " << what << " — event " << event << " has only "
              << daughters.size() << " daughters\n";
    return;
  }
  check(what, daughters[daughter].pdg, expected);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <eventcalc_sample.dat>\n";
    return 2;
  }
  aegir::eventcalc::Reader const reader{argv[1]};

  // Summary: every value is followed by a full stop in the source line, which
  // an istream-based parse would not reliably reject.
  auto const& s = reader.summary();
  check("sampled_events", s.sampled_events, 4.0);
  check("coupling_squared", s.coupling_squared, 1e-10);
  check("total_llps_produced", s.total_llps_produced, 1.5e12);
  check("polar_acceptance", s.polar_acceptance, 0.25);
  check("azimuthal_acceptance", s.azimuthal_acceptance, 0.5);
  check("mean_decay_probability", s.mean_decay_probability, 2e-05);
  check("visible_branching_ratio", s.visible_branching_ratio, 0.8);
  check("total_events", s.total_events, 12.3);

  check("event count", reader.size(), std::size_t{6});

  // Process names contain "->", so the name must terminate at the semicolon,
  // not at the first '>'. The last block header omits sample_points, leaving
  // the closing bracket as the only terminator.
  check("process of event 0", reader.process_name(reader.at(0)),
        std::string("N2 -> e- pi+"));
  check("process of event 2", reader.process_name(reader.at(2)),
        std::string("N2 -> mu- mu+"));
  check("process of event 4", reader.process_name(reader.at(4)),
        std::string("N2 -> nu nubar"));

  // Padding groups must be dropped whole. Testing the sentinel value instead
  // of the group leaves five zeros behind and shifts every later group.
  check("daughters of event 0", reader.at(0).daughters.size(), std::size_t{3});
  check("daughters of event 2", reader.at(2).daughters.size(), std::size_t{2});
  check("daughters of event 3", reader.at(3).daughters.size(), std::size_t{1});
  check("daughters of event 4", reader.at(4).daughters.size(), std::size_t{1});
  // A trailing group with fewer than six columns is dropped, but the event is
  // kept and its weight still counts: aborting a run over one malformed line
  // would be worse than losing the incomplete particle.
  check("daughters of event 5 (partial group dropped)",
        reader.at(5).daughters.size(), std::size_t{1});

  check("event 0 LLP PDG", reader.at(0).llp.pdg, 9900015);
  check("event 0 weight", reader.at(0).decay_probability, 0.001);
  check("event 0 vertex z [m]", reader.at(0).vertex[2], 45.0);
  // Anti-baryons sit far below the -999 padding sentinel, so a threshold test
  // would truncate the daughter list here rather than keep this particle.
  check_daughter("event 0 daughter 2 PDG (anti-proton)", reader, 0, 2, -2212);
  check_daughter("event 2 daughter 1 PDG", reader, 2, 1, -13);
  check("event 3 weight", reader.at(3).decay_probability, 0.004);
  check("summed weight", reader.summed_decay_probability(),
        0.001 + 0.002 + 0.003 + 0.004 + 0.005 + 0.006);

  if (failures == 0) std::cout << "test_eventcalc_reader: all checks passed\n";
  return failures == 0 ? 0 : 1;
}
