// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// scoring_planes.hpp — Scoring planes in a Geant4 parallel world
//
// A scoring plane is a thin box or cylindrical shell that records every track
// entering it — charged or neutral, neutrinos included, whether or not it
// deposits energy — as a SimHit, and can optionally stop the tracks it
// records. This is aegir's counterpart of FairShip's exitHadronAbsorber.
//
// The planes live in their own parallel world (G4VUserParallelWorld), overlaid
// on whatever geometry the provider built (builtin, GDML, GeoModel). Geant4
// tracks particles through both worlds at once, so a plane can be any size,
// anywhere: it may cut through or extend beyond the detector's own volumes
// without disturbing their navigation, and without taking on or changing any
// material (the parallel world is not a layered-mass world). Planes may not
// overlap one another. They work independently of the sd_mode used for the
// geometry's own sensitive volumes.

#pragma once

#include <spdlog/spdlog.h>

#include <G4Box.hh>
#include <G4LogicalVolume.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4Navigator.hh>
#include <G4NistManager.hh>
#include <G4PVPlacement.hh>
#include <G4Point3D.hh>
#include <G4SDManager.hh>
#include <G4Step.hh>
#include <G4StepPoint.hh>
#include <G4String.hh>
#include <G4ThreeVector.hh>
#include <G4Track.hh>
#include <G4Transform3D.hh>
#include <G4TransportationManager.hh>
#include <G4Tubs.hh>
#include <G4VPhysicalVolume.hh>
#include <G4VSensitiveDetector.hh>
#include <G4VSolid.hh>
#include <G4VUserParallelWorld.hh>
#include <SHiP/QuantityView.hpp>
#include <SHiP/SimHit.hpp>
#include <SHiP/Units.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "geant4_sim_core.hpp"
#include "units/clhep_bridge.hpp"

namespace SHiP::g4 {

/// SimHit::detectorId of every scoring-plane hit. Kept clear of the SHiP
/// subsystem ids (SHiP::detector_id, 0–4) and of the sensitive-volume indices
/// the geometry's own sensitive detectors write, so a plane hit cannot be
/// mistaken for a detector hit. Which plane recorded the hit is stored in
/// SimHit::geometryNodeId (the plane's configured `id`).
inline constexpr std::int32_t kScoringPlaneDetectorId = 100;

/// Name of the parallel world holding the scoring planes, and of the
/// G4ParallelWorldPhysics constructor that makes Geant4 track through it.
inline constexpr char const* kScoringWorldName = "aegir_scoring_planes";

enum class ScoringShape : std::uint8_t { box, tube };

/// One scoring plane, as configured in the geant4 module's `scoring_planes`.
/// Both shapes are aligned with the global axes; a tube's axis is along z.
struct ScoringPlaneConfig {
  std::string name;     ///< Volume name; must not exist in the geometry yet
  std::int32_t id = 0;  ///< Written to SimHit::geometryNodeId; unique
  ScoringShape shape = ScoringShape::box;
  ship::Length half_x = ship::Length::zero();  ///< box only
  ship::Length half_y = ship::Length::zero();  ///< box only
  ship::Length r_min = ship::Length::zero();   ///< tube only
  ship::Length r_max = ship::Length::zero();   ///< tube only
  ship::Length half_z = ship::Length::zero();  ///< both shapes
  // Placement — exactly one of:
  //  * `position`: centre in the global frame;
  //  * `after_volume`: downstream of the detector-geometry logical volume
  //    with that exact name. The plane is centred transversely on the
  //    volume's global bounding box and its upstream face sits `gap` behind
  //    the box's downstream end — FairShip's "1 cm after the absorber".
  std::optional<ship::Vec3<ship::Length>> position;
  std::string after_volume;
  ship::Length gap = ship::Length::zero();
  /// Record only tracks entering with at least this kinetic energy.
  ship::Energy ke_threshold = ship::Energy::zero();
  /// Stop and kill every track the plane records (FairShip's StopTrack()).
  bool kill = false;
};

/// What the sensitive detector needs to know about a placed plane.
struct ScoringPlaneInfo {
  std::int32_t id = 0;
  double ke_threshold = 0;  // Geant4 internal units
  bool kill = false;
};

using ScoringPlaneMap = std::unordered_map<G4LogicalVolume*, ScoringPlaneInfo>;

namespace scoring_plane_detail {

/// What is wrong with a plane's shape and size, or an empty string.
inline std::string shape_error(ScoringPlaneConfig const& p) {
  auto const zero = ship::Length::zero();
  if (!(p.half_z > zero)) {
    return "half_z must be positive";
  }
  if (p.shape == ScoringShape::box) {
    if (!(p.half_x > zero) || !(p.half_y > zero)) {
      return "a box needs positive half_x and half_y";
    }
    if (p.r_min != zero || p.r_max != zero) {
      return "r_min/r_max apply to tubes only";
    }
    return {};
  }
  if (!(p.r_min >= zero) || !(p.r_max > p.r_min)) {
    return "a tube needs 0 <= r_min < r_max";
  }
  if (p.half_x != zero || p.half_y != zero) {
    return "half_x/half_y apply to boxes only";
  }
  return {};
}

/// What is wrong with a plane's placement settings, or an empty string.
inline std::string placement_error(ScoringPlaneConfig const& p) {
  auto const zero = ship::Length::zero();
  if (p.position.has_value() == !p.after_volume.empty()) {
    return "set exactly one of 'position' and 'after_volume'";
  }
  if (p.after_volume.empty() && p.gap != zero) {
    return "'gap' applies to 'after_volume' placement only";
  }
  if (p.gap < zero) {
    return "gap must not be negative";
  }
  return {};
}

}  // namespace scoring_plane_detail

/// Check a set of plane configurations for consistency on their own (no
/// geometry needed); throws std::invalid_argument naming the offending plane.
inline void validate_scoring_planes(
    std::vector<ScoringPlaneConfig> const& planes) {
  std::unordered_set<std::string> names;
  std::unordered_set<std::int32_t> ids;
  for (auto const& p : planes) {
    if (p.name.empty()) {
      throw std::invalid_argument("scoring plane without a name");
    }
    std::string error;
    if (!names.insert(p.name).second) {
      error = "name used by more than one plane";
    } else if (!ids.insert(p.id).second) {
      error = "id " + std::to_string(p.id) + " used by more than one plane";
    } else if (p.ke_threshold < ship::Energy::zero()) {
      error = "ke_threshold must not be negative";
    } else if (error = scoring_plane_detail::shape_error(p); error.empty()) {
      error = scoring_plane_detail::placement_error(p);
    }
    if (!error.empty()) {
      throw std::invalid_argument("scoring plane '" + p.name + "': " + error);
    }
  }
}

namespace scoring_plane_detail {

/// A physical volume reached from the world, with the transform taking its
/// local frame to the global one.
struct Placement {
  G4VPhysicalVolume* pv = nullptr;
  G4VPhysicalVolume* parent = nullptr;  // nullptr for the world
  G4Transform3D to_global = G4Transform3D::Identity;
};

/// Depth-first visit of every placement below (and including) `pv`.
/// Replicated and parameterised volumes have no single transform, so they and
/// their contents are not visited.
template <typename Visit>
void walk(G4VPhysicalVolume* pv, G4VPhysicalVolume* parent,
          G4Transform3D const& to_global, Visit& visit) {
  visit(Placement{.pv = pv, .parent = parent, .to_global = to_global});
  auto* lv = pv->GetLogicalVolume();
  for (std::size_t i = 0; i < lv->GetNoDaughters(); ++i) {
    auto* daughter = lv->GetDaughter(static_cast<G4int>(i));
    if (daughter->IsReplicated()) {
      continue;
    }
    walk(daughter, pv,
         to_global * G4Transform3D(daughter->GetObjectRotationValue(),
                                   daughter->GetObjectTranslation()),
         visit);
  }
}

/// The single placement of the logical volume named `lv_name`. A volume
/// placed more than once is rejected: a plane added to (or positioned from)
/// it would be ambiguous or silently duplicated in every copy.
inline Placement find_unique(G4VPhysicalVolume* world,
                             std::string const& lv_name,
                             std::string const& plane, char const* role) {
  std::vector<Placement> found;
  auto visit = [&](Placement const& p) {
    if (p.pv->GetLogicalVolume()->GetName() == lv_name) {
      found.push_back(p);
    }
  };
  walk(world, nullptr, G4Transform3D{}, visit);
  if (found.size() != 1) {
    throw std::runtime_error(
        "scoring plane '" + plane + "': " + role + " '" + lv_name +
        "' must be placed exactly once in the geometry, found " +
        std::to_string(found.size()) +
        " placement(s) (volumes inside replicas are not searched)");
  }
  return found.front();
}

/// Global axis-aligned bounding box of a placed volume.
inline std::pair<G4ThreeVector, G4ThreeVector> global_bounds(
    Placement const& p) {
  G4ThreeVector lo;
  G4ThreeVector hi;
  p.pv->GetLogicalVolume()->GetSolid()->BoundingLimits(lo, hi);
  constexpr double inf = std::numeric_limits<double>::infinity();
  G4ThreeVector gmin{inf, inf, inf};
  G4ThreeVector gmax{-inf, -inf, -inf};
  for (int corner = 0; corner < 8; ++corner) {
    G4Point3D const local{(corner & 1) != 0 ? hi.x() : lo.x(),
                          (corner & 2) != 0 ? hi.y() : lo.y(),
                          (corner & 4) != 0 ? hi.z() : lo.z()};
    G4Point3D const global = p.to_global * local;
    for (int axis = 0; axis < 3; ++axis) {
      gmin[axis] = std::min(gmin[axis], global[axis]);
      gmax[axis] = std::max(gmax[axis], global[axis]);
    }
  }
  return {gmin, gmax};
}

}  // namespace scoring_plane_detail

/// Build the configured planes and place them in `scoring_world`, the
/// parallel world; `after_volume` references are resolved in the detector
/// geometry under `mass_world`. Must run on the geometry-creating thread before
/// the geometry is closed (G4VUserParallelWorld::Construct()). Throws on an
/// ambiguous or missing reference volume, a name clash, or a plane that
/// overlaps another plane or extends beyond the world.
inline ScoringPlaneMap place_scoring_planes(
    G4VPhysicalVolume* mass_world, G4VPhysicalVolume* scoring_world,
    std::vector<ScoringPlaneConfig> const& planes) {
  namespace cb = aegir::clhep;
  validate_scoring_planes(planes);

  ScoringPlaneMap placed;
  if (planes.empty()) {
    return placed;
  }
  auto* vacuum = G4NistManager::Instance()->FindOrBuildMaterial("G4_Galactic");

  for (auto const& cfg : planes) {
    if (G4LogicalVolumeStore::GetInstance()->GetVolume(cfg.name, false) !=
        nullptr) {
      throw std::runtime_error("scoring plane '" + cfg.name +
                               "': the geometry already has a volume with "
                               "this name");
    }

    // The plane's centre in the global frame.
    G4ThreeVector centre;
    if (cfg.position) {
      auto const& pos = *cfg.position;
      centre = {cb::g4(pos[0]), cb::g4(pos[1]), cb::g4(pos[2])};
    } else {
      auto const ref = scoring_plane_detail::find_unique(
          mass_world, cfg.after_volume, cfg.name, "after_volume");
      if (ref.parent == nullptr) {
        throw std::runtime_error("scoring plane '" + cfg.name +
                                 "': after_volume is the world volume");
      }
      auto const [lo, hi] = scoring_plane_detail::global_bounds(ref);
      centre = {0.5 * (lo.x() + hi.x()), 0.5 * (lo.y() + hi.y()),
                hi.z() + cb::g4(cfg.gap) + cb::g4(cfg.half_z)};
    }

    G4VSolid* solid = nullptr;
    if (cfg.shape == ScoringShape::box) {
      solid = new G4Box(cfg.name, cb::g4(cfg.half_x), cb::g4(cfg.half_y),
                        cb::g4(cfg.half_z));
    } else {
      solid = new G4Tubs(cfg.name, cb::g4(cfg.r_min), cb::g4(cfg.r_max),
                         cb::g4(cfg.half_z), 0.,
                         2 * std::numbers::pi);  // Geant4 angles are in rad
    }
    auto* lv = new G4LogicalVolume(solid, vacuum, cfg.name);

    // Axis-aligned in the global frame. The parallel world sits where the
    // detector world does (normally at the origin).
    G4Transform3D const in_world =
        G4Transform3D(scoring_world->GetObjectRotationValue(),
                      scoring_world->GetObjectTranslation())
            .inverse() *
        G4Translate3D(centre);
    auto* pv = new G4PVPlacement(in_world, lv, cfg.name,
                                 scoring_world->GetLogicalVolume(), false, 0);
    // Only other planes and the world boundary can be overlapped here: the
    // detector geometry is in a different world.
    if (pv->CheckOverlaps()) {
      throw std::runtime_error(
          "scoring plane '" + cfg.name +
          "' overlaps another scoring plane or extends beyond the world");
    }

    placed.emplace(lv,
                   ScoringPlaneInfo{.id = cfg.id,
                                    .ke_threshold = cb::g4(cfg.ke_threshold),
                                    .kill = cfg.kill});
    auto const mm = [](double v) {
      return cb::length(v).numerical_value_in(ship::units::mm);
    };
    spdlog::info(
        "geant4: scoring plane '{}' (id {}) placed, centre ({:.1f}, {:.1f}, "
        "{:.1f}) mm{}",
        cfg.name, cfg.id, mm(centre.x()), mm(centre.y()), mm(centre.z()),
        cfg.kill ? ", kills recorded tracks" : "");
  }
  return placed;
}

/// Records one SimHit per track entering a scoring plane through its surface,
/// regardless of particle type or energy deposit. detectorId is
/// kScoringPlaneDetectorId, geometryNodeId the plane's id; position, momentum
/// and time are taken at the entry point. Called by G4ParallelWorldProcess
/// with the parallel-world step: its pre-step point carries the plane's
/// touchable and, on entry, fGeomBoundary; position, momentum and time are the
/// track's own.
class ScoringPlaneSD : public G4VSensitiveDetector {
 public:
  ScoringPlaneSD(G4String const& name, ScoringPlaneMap planes)
      : G4VSensitiveDetector(name), planes_{std::move(planes)} {}

  G4bool ProcessHits(G4Step* step, G4TouchableHistory*) override {
    auto const* pre = step->GetPreStepPoint();
    // Only tracks entering through the surface (FairShip's
    // IsTrackEntering()): not later steps inside the plane, and not tracks
    // created inside it.
    if (pre->GetStepStatus() != fGeomBoundary) {
      return false;
    }
    auto const it =
        planes_.find(pre->GetTouchable()->GetVolume()->GetLogicalVolume());
    if (it == planes_.end()) {
      return false;
    }
    auto const& plane = it->second;
    if (pre->GetKineticEnergy() < plane.ke_threshold) {
      return false;
    }

    auto* track = step->GetTrack();
    auto hit = make_base_hit(step, kScoringPlaneDetectorId);
    hit.geometryNodeId = plane.id;
    hit.energyDeposit = 0;
    ship::view::setPathLength(hit,
                              aegir::clhep::length(track->GetTrackLength()));
    tl_hits.push_back(hit);

    if (plane.kill) {
      track->SetTrackStatus(fStopAndKill);
    }
    return true;
  }

 private:
  ScoringPlaneMap planes_;
};

/// The parallel world holding the scoring planes. Registered with the
/// detector construction when planes are configured; Geant4 calls Construct()
/// once on the geometry thread and ConstructSD() on every thread that tracks.
/// The run also needs G4ParallelWorldPhysics(kScoringWorldName) in its physics
/// list, so that particles are transported through this world.
class ScoringParallelWorld : public G4VUserParallelWorld {
 public:
  explicit ScoringParallelWorld(std::vector<ScoringPlaneConfig> planes)
      : G4VUserParallelWorld(kScoringWorldName), configs_{std::move(planes)} {}

  void Construct() override {
    auto* mass_world = G4TransportationManager::GetTransportationManager()
                           ->GetNavigatorForTracking()
                           ->GetWorldVolume();
    auto* scoring_world = GetWorld();
    world_lv_ = scoring_world->GetLogicalVolume();
    planes_ = place_scoring_planes(mass_world, scoring_world, configs_);
  }

  void ConstructSD() override {
    if (planes_.empty()) {
      return;
    }
    auto* sd = new ScoringPlaneSD("ScoringPlaneSD", planes_);
    G4SDManager::GetSDMpointer()->AddNewDetector(sd);
    for (auto const& [lv, _] : planes_) {
      lv->SetSensitiveDetector(sd);
    }
  }

  /// Whether `lv` belongs to this world (a plane or the world volume), so
  /// that detector-geometry patterns (sensitive volumes, field regions) can
  /// skip it. Valid once Construct() has run.
  [[nodiscard]] bool contains(G4LogicalVolume* lv) const {
    return lv == world_lv_ || planes_.contains(lv);
  }

 private:
  std::vector<ScoringPlaneConfig> configs_;
  // Written by Construct() on the geometry thread during master
  // initialisation, read by every thread afterwards.
  ScoringPlaneMap planes_;
  G4LogicalVolume* world_lv_ = nullptr;
};

}  // namespace SHiP::g4
