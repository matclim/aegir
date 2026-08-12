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
  ship::Energy ke_threshold_;  // used by CrossingSD
  std::vector<std::pair<std::string, ship::Length>>
      regions_;  // pattern -> production cut

 public:
  ConfigurableDetectorConstruction(
      IGeometrySource const& source, ship::IFieldSource const& field_source,
      SDMode sd_mode = SDMode::scoring,
      ship::Energy ke_threshold = ship::Energy::zero(),
      std::vector<std::pair<std::string, ship::Length>> regions = {})
      : source_{&source},
        field_source_{&field_source},
        sd_mode_{sd_mode},
        ke_threshold_{ke_threshold},
        regions_{std::move(regions)} {}

  // Called once, on ship::geometry_thread() (the run manager is initialised
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
      cuts->SetProductionCut(aegir::clhep::g4(cut));
      region->SetProductionCuts(cuts);
      bool matched = false;
      for (auto* lv : *G4LogicalVolumeStore::GetInstance()) {
        if (G4StrUtil::contains(lv->GetName(), std::string_view{pattern})) {
          region->AddRootLogicalVolume(lv);
          matched = true;
        }
      }
      if (!matched)
        throw std::runtime_error("Production-cut region '" + pattern +
                                 "' matches no logical volumes");
    }

    return world;
  }

  void ConstructSDandField() override {
    auto const& sv_names = source_->sensitiveVolumes();
    DetectorIdMap detector_ids;

    std::vector<bool> matched(sv_names.size(), false);
    for (auto* lv : *G4LogicalVolumeStore::GetInstance()) {
      for (int i = 0; i < static_cast<int>(sv_names.size()); ++i) {
        if (G4StrUtil::contains(lv->GetName(), std::string_view{sv_names[i]})) {
          detector_ids.emplace(lv, i);
          matched[i] = true;
          break;
        }
      }
    }

    // A pattern matching nothing used to fail silently: the run completed and
    // simply produced no hits for that detector, which is indistinguishable
    // from the particles having missed it. Treated as a configuration error,
    // as unmatched field-magnet patterns already are below.
    for (std::size_t i = 0; i < sv_names.size(); ++i) {
      if (!matched[i]) {
        throw std::runtime_error("Sensitive volume pattern '" + sv_names[i] +
                                 "' matches no logical volumes");
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
  }
};

}  // namespace SHiP::g4
