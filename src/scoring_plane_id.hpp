// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// scoring_plane_id.hpp — the detector id of scoring-plane hits
//
// Kept free of Geant4 so that consumers of SimHit, such as the output
// module's validation histograms, can tell scoring-plane hits apart.

#pragma once

#include <cstdint>

namespace SHiP::g4 {

/// SimHit::detectorId of every scoring-plane hit. Kept clear of the SHiP
/// subsystem ids (SHiP::detector_id, 0–4) and of the sensitive-volume indices
/// the geometry's own sensitive detectors write, so a plane hit cannot be
/// mistaken for a detector hit. Which plane recorded the hit is stored in
/// SimHit::geometryNodeId (the plane's configured `id`).
inline constexpr std::int32_t kScoringPlaneDetectorId = 100;

}  // namespace SHiP::g4
