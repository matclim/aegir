// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// detector_construction.hpp — Bridges IGeometrySource to
// G4VUserDetectorConstruction
//
// Delegates Construct() to IGeometrySource::construct() and assigns the
// configured SD type to volumes matching IGeometrySource::sensitiveVolumes(),
// whose hits carry that entry's detector id.
// Registers the configured scoring planes as a parallel world
// (ScoringParallelWorld, see scoring_planes.hpp).

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

#include "SHiP/FieldService/G4MagFieldAdapter.h"
#include "SHiP/FieldService/IFieldSource.h"
#include "geant4_sim_core.hpp"
#include "geometry_source.hpp"
#include "scoring_planes.hpp"

namespace SHiP::aegir::g4 {

enum class SDMode { scoring, crossing };

class ConfigurableDetectorConstruction : public G4VUserDetectorConstruction {
  IGeometrySource const*
      source_;  // non-owning; Job-layer product outlives the G4 run
  SHiP::IFieldSource const* field_source_;  // non-owning; may have no regions
  SDMode sd_mode_;
  SHiP::Energy ke_threshold_;  // used by CrossingSD
  std::vector<std::pair<std::string, SHiP::Length>>
      regions_;  // pattern -> production cut
  // The scoring planes' parallel world, or null without planes. Owned by
  // G4VUserDetectorConstruction once registered (leaked with it, like the
  // rest of the Geant4 setup).
  ScoringParallelWorld* scoring_world_ = nullptr;

  // The detector geometry's logical volumes: the volume store without the
  // scoring planes' parallel world, which the sensitive-volume and field
  // patterns must not match.
  [[nodiscard]] std::vector<G4LogicalVolume*> detector_volumes() const {
    std::vector<G4LogicalVolume*> volumes;
    for (auto* lv : *G4LogicalVolumeStore::GetInstance()) {
      if (scoring_world_ == nullptr || !scoring_world_->contains(lv)) {
        volumes.push_back(lv);
      }
    }
    return volumes;
  }

 public:
  ConfigurableDetectorConstruction(
      IGeometrySource const& source, SHiP::IFieldSource const& field_source,
      SDMode sd_mode = SDMode::scoring,
      SHiP::Energy ke_threshold = SHiP::Energy::zero(),
      std::vector<std::pair<std::string, SHiP::Length>> regions = {},
      std::vector<ScoringPlaneConfig> scoring_planes = {})
      : source_{&source},
        field_source_{&field_source},
        sd_mode_{sd_mode},
        ke_threshold_{ke_threshold},
        regions_{std::move(regions)} {
    if (!scoring_planes.empty()) {
      scoring_world_ = new ScoringParallelWorld(std::move(scoring_planes));
      RegisterParallelWorld(scoring_world_);
    }
  }

  /// Whether scoring planes are configured, i.e. the run needs
  /// G4ParallelWorldPhysics(kScoringWorldName).
  [[nodiscard]] bool has_scoring_planes() const {
    return scoring_world_ != nullptr;
  }

  // Called once, on SHiP::geometry_thread() (the run manager is initialised
  // there — the process's single Geant4 geometry-creating thread)
  G4VPhysicalVolume* Construct() override {
    auto* world = source_->construct();

    // Create the configured production-cut regions here, not in
    // ConstructSDandField: G4Region is a split class, so regions must be
    // created on the process's single geometry-creating thread — this one —
    // while ConstructSDandField runs on worker threads (#80). Construct()
    // is also the canonical Geant4 place: the regions exist before any
    // worker builds its per-region physics tables.
    for (auto const& [pattern, cut] : regions_) {
      auto* region = new G4Region(pattern);
      auto* cuts = new G4ProductionCuts();
      cuts->SetProductionCut(SHiP::aegir::clhep::g4(cut));
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
    // The detector ids were resolved when the geometry provider read its
    // configuration (read_sensitive_volumes), so nothing here can fail.
    auto const& sensitive_volumes = source_->sensitiveVolumes();
    DetectorIdMap detector_ids;

    auto const volumes = detector_volumes();
    for (auto* lv : volumes) {
      for (auto const& sv : sensitive_volumes) {
        if (G4StrUtil::contains(lv->GetName(), std::string_view{sv.pattern})) {
          detector_ids.emplace(
              lv, SensitiveInfo{.detector_id = sv.detector_id,
                                .copy_number_depth = sv.copy_number_depth});
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

    // Install per-magnet G4FieldManagers on every logical volume whose name
    // contains the configured pattern. Outside these volumes Geant4 never
    // invokes the field, so drift regions stay cost-free. An unmatched
    // pattern is treated as a hard configuration error (wrong field setup is
    // a silent physics bug otherwise).
    for (auto const& fr : field_source_->regions()) {
      auto* adapter = new SHiP::G4MagFieldAdapter(fr.field);
      auto* fmgr = new G4FieldManager(adapter);
      bool matched = false;
      for (auto* lv : volumes) {
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

}  // namespace SHiP::aegir::g4
