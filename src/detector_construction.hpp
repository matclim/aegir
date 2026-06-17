// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// detector_construction.hpp — Bridges IGeometrySource to
// G4VUserDetectorConstruction
//
// Delegates Construct() to IGeometrySource::construct() and assigns the
// configured SD type to volumes matching IGeometrySource::sensitiveVolumes().

#pragma once

#include <G4FieldManager.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4ProductionCuts.hh>
#include <G4Region.hh>
#include <G4SDManager.hh>
#include <G4String.hh>
#include <G4VUserDetectorConstruction.hh>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "FieldService/G4MagFieldAdapter.h"
#include "FieldService/IFieldSource.h"
#include "geant4_sim_core.hpp"
#include "geometry_source.hpp"

namespace SHiP::g4 {

enum class SDMode { scoring, crossing };

class ConfigurableDetectorConstruction : public G4VUserDetectorConstruction {
  IGeometrySource const*
      source_;  // non-owning; Job-layer product outlives the G4 run
  ship::IFieldSource const* field_source_;  // non-owning; may have no regions
  SDMode sd_mode_;
  double ke_threshold_;  // GeV, used by CrossingSD
  std::vector<std::pair<std::string, double>> regions_;  // pattern -> cut in mm
  std::once_flag regions_flag_;

 public:
  ConfigurableDetectorConstruction(
      IGeometrySource const& source, ship::IFieldSource const& field_source,
      SDMode sd_mode = SDMode::scoring, double ke_threshold = 0.0,
      std::vector<std::pair<std::string, double>> regions = {})
      : source_{&source},
        field_source_{&field_source},
        sd_mode_{sd_mode},
        ke_threshold_{ke_threshold},
        regions_{std::move(regions)} {}

  // Called once, master thread only
  G4VPhysicalVolume* Construct() override { return source_->construct(); }

  void ConstructSDandField() override {
    auto const& sv_names = source_->sensitiveVolumes();
    DetectorIdMap detector_ids;

    for (auto* lv : *G4LogicalVolumeStore::GetInstance()) {
      for (int i = 0; i < static_cast<int>(sv_names.size()); ++i) {
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
      if (!matched)
        throw std::runtime_error("Field region '" + fr.name +
                                 "': volume_pattern '" + fr.volume_pattern +
                                 "' matches no logical volumes");
    }

    // Create G4Regions with custom production cuts (once only)
    std::call_once(regions_flag_, [this]() {
      for (auto const& [pattern, cut_mm] : regions_) {
        auto* region = new G4Region(pattern);
        auto* cuts = new G4ProductionCuts();
        cuts->SetProductionCut(cut_mm * mm);
        region->SetProductionCuts(cuts);
        for (auto* lv : *G4LogicalVolumeStore::GetInstance()) {
          if (G4StrUtil::contains(lv->GetName(), std::string_view{pattern}))
            region->AddRootLogicalVolume(lv);
        }
      }
    });
  }
};

}  // namespace SHiP::g4
