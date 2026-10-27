// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// sensitive_volumes_test.cpp — detector ids of sensitive_volumes entries

#include "sensitive_volumes.hpp"

#include <SHiP/detectors/detector_id.hpp>
#include <boost/json.hpp>
#include <cstdint>
#include <format>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phlex/configuration.hpp"

namespace {

// Counts and reports failed checks.
struct Checker {
  int failures = 0;

  void operator()(bool ok, std::string const& what) {
    if (!ok) {
      std::println(stderr, "FAILED: {}", what);
      ++failures;
    }
  }
};

std::int32_t id(SHiP::detector_id d) { return static_cast<std::int32_t>(d); }

phlex::configuration config(std::string_view json) {
  return phlex::configuration{boost::json::parse(json).as_object()};
}

// Whether reading `json` throws std::invalid_argument.
bool rejects(std::string_view json) {
  try {
    (void)SHiP::aegir::read_sensitive_volumes(config(json));
  } catch (std::invalid_argument const&) {
    return true;
  }
  return false;
}

}  // namespace

int main() {
  using SHiP::aegir::detector_id_for_pattern;
  using SHiP::aegir::detector_id_from_name;
  using SHiP::aegir::kTestFixtureDetectorId;
  using SHiP::aegir::read_sensitive_volumes;
  using enum SHiP::detector_id;
  Checker check;

  for (auto const& [pattern, expected] :
       std::vector<std::pair<std::string_view, std::int32_t>>{
           {"UpstreamTagger", id(UpstreamTagger)},
           {"upstream_tagger", id(UpstreamTagger)},
           {"SurroundTagger", id(SurroundTagger)},
           {"StrawTubes", id(StrawTubes)},
           {"trackers", id(StrawTubes)},
           {"Calorimeter", id(Calorimeter)},
           {"calorimeter", id(Calorimeter)},
           // Both used to come out as Calorimeter.
           {"TimingDetector", id(TimingDetector)},
           {"timing_detector", id(TimingDetector)},
           {"Scoring", kTestFixtureDetectorId},
           {"ScoringPlane", kTestFixtureDetectorId},
       }) {
    check(detector_id_for_pattern(pattern) == expected,
          std::format("pattern {}", pattern));
  }

  bool threw = false;
  try {
    (void)detector_id_for_pattern("StrawTube");
  } catch (std::invalid_argument const&) {
    threw = true;
  }
  check(threw, "an unknown pattern is an error");

  check(detector_id_from_name("TimingDetector") == id(TimingDetector),
        "detector name");
  check(detector_id_from_name("test_fixture") == kTestFixtureDetectorId,
        "test_fixture name");
  threw = false;
  try {
    (void)detector_id_from_name("trackers");
  } catch (std::invalid_argument const&) {
    threw = true;
  }
  check(threw, "detector names are not patterns");

  auto const mixed = read_sensitive_volumes(config(R"({"sensitive_volumes": [
      "trackers",
      {"pattern": "straw_gas", "detector": "StrawTubes"},
      {"pattern": "bar", "detector": "TimingDetector"}]})"));
  check(mixed.size() == 3, "mixed: every entry is read");
  check(mixed.size() == 3 && mixed[0].pattern == "trackers" &&
            mixed[0].detector_id == id(StrawTubes),
        "mixed: pattern entry");
  check(mixed.size() == 3 && mixed[1].pattern == "straw_gas" &&
            mixed[1].detector_id == id(StrawTubes),
        "mixed: two entries may share a detector");
  check(mixed.size() == 3 && mixed[2].pattern == "bar" &&
            mixed[2].detector_id == id(TimingDetector),
        "mixed: explicit entry");

  check(mixed.size() == 3 && mixed[0].copy_number_depth == -1 &&
            mixed[1].copy_number_depth == -1,
        "mixed: no copy number by default");

  auto const copies = read_sensitive_volumes(config(R"({"sensitive_volumes": [
      {"pattern": "gas", "detector": "StrawTubes", "copy_number_depth": 0},
      {"pattern": "VSensitive", "detector": "test_fixture",
       "copy_number_depth": 2}]})"));
  check(copies.size() == 2 && copies[0].copy_number_depth == 0 &&
            copies[1].copy_number_depth == 2 &&
            copies[1].detector_id == kTestFixtureDetectorId,
        "copy_number_depth");
  check(rejects(R"({"sensitive_volumes": [{"pattern": "gas",
                    "detector": "StrawTubes", "copy_number_depth": -1}]})"),
        "a negative copy_number_depth");

  check(read_sensitive_volumes(config(R"({"sensitive_volumes": []})")).empty(),
        "an empty list");

  // phlex's own error for a missing key.
  threw = false;
  try {
    (void)read_sensitive_volumes(config("{}"));
  } catch (std::runtime_error const&) {
    threw = true;
  }
  check(threw, "a missing sensitive_volumes");
  check(rejects(R"({"sensitive_volumes": ["StrawTube"]})"),
        "an unknown pattern");
  check(rejects(R"({"sensitive_volumes": [{"detector": "StrawTubes"}]})"),
        "an entry without a pattern");
  check(rejects(R"({"sensitive_volumes": [{"pattern": "straw_gas"}]})"),
        "an entry without a detector");
  check(rejects(R"({"sensitive_volumes":
                    [{"pattern": "straw_gas", "detector": "Straws"}]})"),
        "an unknown detector");
  check(rejects(R"({"sensitive_volumes": [{"pattern": "straw_gas",
                    "detector": "StrawTubes", "name": "straws"}]})"),
        "an unknown key");
  check(rejects(R"({"sensitive_volumes": [42]})"), "a number entry");

  if (check.failures == 0) {
    std::println("sensitive_volumes: all checks passed");
  }
  return check.failures == 0 ? 0 : 1;
}
