// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// detector_construction.hpp — Bridges IGeometrySource to
// G4VUserDetectorConstruction
//
// Delegates Construct() to IGeometrySource::construct() and assigns the
// configured SD type to volumes matching IGeometrySource::sensitiveVolumes().
// Adds the configured scoring planes to the geometry and instruments them
// with ScoringPlaneSD (see scoring_planes.hpp).

#pragma once

#include <G4FieldManager.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4ProductionCuts.hh>
#include <G4Region.hh>
#include <G4SDManager.hh>
#include <G4String.hh>
#include <G4VUserDetectorConstruction.hh>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "FieldService/G4MagFieldAdapter.h"
#include "FieldService/IFieldSource.h"
#include "geant4_sim_core.hpp"
#include "geometry_source.hpp"
#include "scoring_planes.hpp"

namespace SHiP::g4 {

enum class SDMode { scoring, crossing };

class ConfigurableDetectorConstruction : public G4VUserDetectorConstruction {
  IGeometrySource const*
      source_;  // non-owning; Job-layer product outlives the G4 run
  ship::IFieldSource const* field_source_;  // non-owning; may have no regions
  SDMode sd_mode_;
  ship::Energy ke_threshold_;  // used by CrossingSD
  std::vector<std::pair<std::string, ship::Length>>
      regions_;  // pattern -> production cut
  std::vector<ScoringPlaneConfig> scoring_plane_cfgs_;
  // Filled by Construct() on the geometry thread; read by every worker's
  // ConstructSDandField(), which only runs after master initialisation.
  ScoringPlaneMap scoring_planes_;

 public:
  ConfigurableDetectorConstruction(
      IGeometrySource const& source, ship::IFieldSource const& field_source,
      SDMode sd_mode = SDMode::scoring,
      ship::Energy ke_threshold = ship::Energy::zero(),
      std::vector<std::pair<std::string, ship::Length>> regions = {},
      std::vector<ScoringPlaneConfig> scoring_planes = {})
      : source_{&source},
        field_source_{&field_source},
        sd_mode_{sd_mode},
        ke_threshold_{ke_threshold},
        regions_{std::move(regions)},
        scoring_plane_cfgs_{std::move(scoring_planes)} {}

  // Called once, on ship::geometry_thread() (the run manager is initialised
  // there — the process's single Geant4 geometry-creating thread)
  G4VPhysicalVolume* Construct() override {
    auto* world = source_->construct();

    // Scoring planes go in first, while the geometry is still open, so the
    // region patterns below may also match them.
    scoring_planes_ = place_scoring_planes(world, scoring_plane_cfgs_);

    // Create the configured production-cut regions here, not in
    // ConstructSDandField: G4Region is a split class, so regions must be
    // created on the process's single geometry-creating thread — this one —
    // while ConstructSDandField runs on worker threads (#80). Construct()
    // is also the canonical Geant4 place: the regions exist before any
    // worker builds its per-region physics tables.
    for (auto const& [pattern, cut] : regions_) {
      auto* region = new G4Region(pattern);
      auto* cuts = new G4ProductionCuts();
      cuts->SetProductionCut(aegir::clhep::g4(cut));
      region->SetProductionCuts(cuts);
      bool matched = false;
      for (auto* lv : *G4LogicalVolumeStore::GetInstance()) {
        if (G4StrUtil::contains(lv->GetName(), std::string_view{pattern})) {
          region->AddRootLogicalVolume(lv);
          matched = true;
        }
      }
      if (!matched) {
        throw std::runtime_error("Production-cut region '" + pattern +
                                 "' matches no logical volumes");
      }
    }

    return world;
  }

  void ConstructSDandField() override {
    auto const& sv_names = source_->sensitiveVolumes();
    DetectorIdMap detector_ids;

    for (auto* lv : *G4LogicalVolumeStore::GetInstance()) {
      if (scoring_planes_.contains(lv)) {
        continue;  // instrumented by ScoringPlaneSD below
      }
      for (int i = 0; std::cmp_less(i, sv_names.size()); ++i) {
        if (G4StrUtil::contains(lv->GetName(), std::string_view{sv_names[i]})) {
          detector_ids.emplace(lv, i);
          break;
        }
      }
    }

    G4VSensitiveDetector* sd = nullptr;
    if (sd_mode_ == SDMode::crossing) {
      sd = new CrossingSD("CrossingSD", detector_ids, ke_threshold_);
    } else {
      sd = new ScoringSD("ScoringSD", detector_ids);
    }
    G4SDManager::GetSDMpointer()->AddNewDetector(sd);

    for (auto [lv, _] : detector_ids) {
      lv->SetSensitiveDetector(sd);
    }

    if (!scoring_planes_.empty()) {
      auto* plane_sd = new ScoringPlaneSD("ScoringPlaneSD", scoring_planes_);
      G4SDManager::GetSDMpointer()->AddNewDetector(plane_sd);
      for (auto const& [lv, _] : scoring_planes_) {
        lv->SetSensitiveDetector(plane_sd);
      }
    }

    // Install per-magnet G4FieldManagers on every logical volume whose name
    // contains the configured pattern. Outside these volumes Geant4 never
    // invokes the field, so drift regions stay cost-free. An unmatched
    // pattern is treated as a hard configuration error (wrong field setup is
    // a silent physics bug otherwise).
    for (auto const& fr : field_source_->regions()) {
      auto* adapter = new ship::G4MagFieldAdapter(fr.field);
      auto* fmgr = new G4FieldManager(adapter);
      bool matched = false;
      for (auto* lv : *G4LogicalVolumeStore::GetInstance()) {
        if (G4StrUtil::contains(lv->GetName(),
                                std::string_view{fr.volume_pattern})) {
          lv->SetFieldManager(fmgr, /*forceToAllDaughters=*/true);
          matched = true;
        }
      }
      if (!matched) {
        throw std::runtime_error("Field region '" + fr.name +
                                 "': volume_pattern '" + fr.volume_pattern +
                                 "' matches no logical volumes");
      }
    }
  }
};

}  // namespace SHiP::g4
