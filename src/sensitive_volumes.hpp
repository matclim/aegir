// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// sensitive_volumes.hpp — the geometry's sensitive volumes and their
// detector ids
//
// Reads a geometry provider's `sensitive_volumes` list and resolves each
// entry's SimHit::detector_id once, at configuration time, so that a bad
// entry fails before any Geant4 object exists. Kept free of Geant4.

#pragma once

#include <SHiP/detectors/detector_id.hpp>
#include <boost/json.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phlex/configuration.hpp"

namespace SHiP::aegir {

/// One `sensitive_volumes` entry: the logical volumes whose names contain
/// `pattern` get a sensitive detector, and their hits carry `detector_id`.
/// With `copy_number_depth` >= 0, a hit also carries the copy number of the
/// volume that many levels above the hit's volume (0: the volume itself) as
/// SimHit::geometry_node_id, telling the channels of a GDML geometry apart.
struct SensitiveVolume {
  std::string pattern;
  std::int32_t detector_id;
  std::int32_t copy_number_depth = -1;  ///< -1: geometry_node_id stays 0
};

/// SimHit::detector_id of hits in the toy geometries' "Scoring" planes (the
/// builtin geometry and the target-only GDML), which belong to no SHiP
/// subsystem. Kept clear of SHiP::detector_id (0-4), of the -1 of a volume
/// without an id and of the scoring planes' 100, and inside the output
/// module's h_hit_detector range.
inline constexpr std::int32_t kTestFixtureDetectorId = 99;

/// The detector id of a bare `sensitive_volumes` pattern. The correspondence
/// between a volume-name pattern and a SHiP::detector_id is a fact about the
/// geometry, so the known patterns are listed here rather than restated in
/// every workflow.
[[nodiscard]] inline std::int32_t detector_id_for_pattern(
    std::string_view pattern) {
  auto const id = [](SHiP::detector_id d) {
    return static_cast<std::int32_t>(d);
  };
  if (pattern == "UpstreamTagger" || pattern == "upstream_tagger") {
    return id(SHiP::detector_id::UpstreamTagger);
  }
  if (pattern == "SurroundTagger") {
    return id(SHiP::detector_id::SurroundTagger);
  }
  if (pattern == "StrawTubes" || pattern == "trackers") {
    return id(SHiP::detector_id::StrawTubes);
  }
  if (pattern == "Calorimeter" || pattern == "calorimeter") {
    return id(SHiP::detector_id::Calorimeter);
  }
  if (pattern == "TimingDetector" || pattern == "timing_detector") {
    return id(SHiP::detector_id::TimingDetector);
  }
  if (pattern == "Scoring" || pattern == "ScoringPlane") {
    return kTestFixtureDetectorId;
  }
  throw std::invalid_argument(
      "sensitive_volumes: no detector id for pattern '" + std::string{pattern} +
      "'");
}

/// The detector id of a `detector` name in a `sensitive_volumes` entry: one
/// of the SHiP::detector_id enumerators, or `test_fixture` for
/// kTestFixtureDetectorId.
[[nodiscard]] inline std::int32_t detector_id_from_name(std::string_view name) {
  using enum SHiP::detector_id;
  if (name == "test_fixture") {
    return kTestFixtureDetectorId;
  }
  for (auto const [known, id] : {std::pair{"UpstreamTagger", UpstreamTagger},
                                 std::pair{"SurroundTagger", SurroundTagger},
                                 std::pair{"StrawTubes", StrawTubes},
                                 std::pair{"Calorimeter", Calorimeter},
                                 std::pair{"TimingDetector", TimingDetector}}) {
    if (name == known) {
      return static_cast<std::int32_t>(id);
    }
  }
  throw std::invalid_argument(
      "unknown detector '" + std::string{name} +
      "' (expected UpstreamTagger, SurroundTagger, StrawTubes, Calorimeter, "
      "TimingDetector or test_fixture)");
}

[[noreturn]] inline void sensitive_volume_error(std::size_t index,
                                                std::string const& what) {
  throw std::invalid_argument("sensitive_volumes entry #" +
                              std::to_string(index) +
                              " (counting from 0): " + what);
}

/// Read the `sensitive_volumes` list of a geometry provider's configuration.
/// Each entry is either a volume-name pattern, whose detector id comes from
/// detector_id_for_pattern(), or `{pattern: ..., detector: ...}`, which names
/// the detector itself. The two forms can be mixed.
[[nodiscard]] inline std::vector<SensitiveVolume> read_sensitive_volumes(
    phlex::configuration const& config) {
  // phlex::configuration only reads lists of one kind, so read the entries
  // as raw JSON values and tell the two forms apart here.
  auto const entries =
      config.get<std::vector<boost::json::value>>("sensitive_volumes");
  std::vector<SensitiveVolume> volumes;
  volumes.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto const& entry = entries[i];
    if (entry.is_string()) {
      std::string pattern{entry.as_string()};
      try {
        auto const detector_id = detector_id_for_pattern(pattern);
        volumes.push_back(
            {.pattern = std::move(pattern), .detector_id = detector_id});
      } catch (std::invalid_argument const&) {
        sensitive_volume_error(
            i, std::format("no detector id is known for pattern '{0}'; write "
                           "it as {{pattern: '{0}', detector: ...}} to name "
                           "the detector",
                           pattern));
      }
      continue;
    }
    if (!entry.is_object()) {
      sensitive_volume_error(
          i, "expected a pattern or {pattern: ..., detector: ...}");
    }
    auto const object = boost::json::value_to<phlex::configuration>(entry);
    for (auto const& key : object.keys()) {
      if (key != "pattern" && key != "detector" &&
          key != "copy_number_depth") {
        sensitive_volume_error(i, "unknown key '" + key + "'");
      }
    }
    auto pattern = object.get_if_present<std::string>("pattern");
    auto const detector = object.get_if_present<std::string>("detector");
    if (!pattern || pattern->empty()) {
      sensitive_volume_error(i, "no 'pattern'");
    }
    if (!detector) {
      sensitive_volume_error(i, "no 'detector' for pattern '" + *pattern + "'");
    }
    std::int32_t detector_id = 0;
    try {
      detector_id = detector_id_from_name(*detector);
    } catch (std::invalid_argument const& e) {
      sensitive_volume_error(i, e.what());
    }
    auto const depth = object.get_if_present<int>("copy_number_depth");
    if (depth && *depth < 0) {
      sensitive_volume_error(i, "copy_number_depth must not be negative");
    }
    volumes.push_back({.pattern = std::move(*pattern),
                       .detector_id = detector_id,
                       .copy_number_depth = depth.value_or(-1)});
  }
  return volumes;
}

}  // namespace SHiP::aegir
