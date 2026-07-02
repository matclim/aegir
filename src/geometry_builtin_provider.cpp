// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geometry_builtin_provider.cpp — Phlex provider plugin for the builtin test
// geometry
//
// Provides the tungsten target + scoring planes geometry as a Job-layer
// product.

#include <G4Box.hh>
#include <G4LogicalVolume.hh>
#include <G4NistManager.hh>
#include <G4PVPlacement.hh>
#include <G4SystemOfUnits.hh>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "geometry_source.hpp"
#include "phlex/source.hpp"
#include "provider_helpers.hpp"

namespace {

class BuiltinGeometrySource : public SHiP::IGeometrySource {
  std::vector<std::string> sensitive_vols_{"Scoring"};
  mutable std::once_flag init_flag_;
  mutable G4VPhysicalVolume* world_ = nullptr;

 public:
  [[nodiscard]] G4VPhysicalVolume* construct() const override {
    std::call_once(init_flag_, [this]() {
      auto* nist = G4NistManager::Instance();

      auto* worldMat = nist->FindOrBuildMaterial("G4_AIR");
      auto* worldBox = new G4Box("World", 5 * m, 5 * m, 20 * m);
      auto* worldLV = new G4LogicalVolume(worldBox, worldMat, "World");
      world_ = new G4PVPlacement(nullptr, G4ThreeVector(), worldLV, "World",
                                 nullptr, false, 0);

      auto* targetMat = nist->FindOrBuildMaterial("G4_W");
      auto* targetBox = new G4Box("Target", 50 * mm, 50 * mm, 500 * mm);
      auto* targetLV = new G4LogicalVolume(targetBox, targetMat, "Target");
      new G4PVPlacement(nullptr, G4ThreeVector(0, 0, 0), targetLV, "Target",
                        worldLV, false, 0);

      auto* siMat = nist->FindOrBuildMaterial("G4_Si");
      auto* planeBox = new G4Box("ScoringPlane", 2 * m, 2 * m, 0.3 * mm);
      auto* planeLV = new G4LogicalVolume(planeBox, siMat, "ScoringPlane");

      std::array<double, 5> z_positions{2 * m, 4 * m, 6 * m, 8 * m, 10 * m};
      for (int i = 0; i < static_cast<int>(z_positions.size()); ++i) {
        new G4PVPlacement(nullptr, G4ThreeVector(0, 0, z_positions[i]), planeLV,
                          "ScoringPlane", worldLV, false, i);
      }
    });
    return world_;
  }

  [[nodiscard]] std::vector<std::string> const& sensitiveVolumes()
      const override {
    return sensitive_vols_;
  }
};

}  // namespace

PHLEX_REGISTER_PROVIDERS(s) {
  using namespace phlex;

  // Shared instance: geometry is constant across all events.
  // The provider returns a shared_ptr copy for each data cell.
  auto source = std::make_shared<BuiltinGeometrySource>();

  // TODO: move to job layer once phlex supports declaring it without
  // self-referential parent (currently segfaults in layer_generator).
  aegir::provide_constant(s, "create_geometry", source, "geometry", "detector",
                          "event");
}
