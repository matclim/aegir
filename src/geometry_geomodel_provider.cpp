// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geometry_geomodel_provider.cpp — Phlex provider plugin for GeoModel geometry
//
// Loads a GeoModel .db file via SHiPGeometryService and converts to Geant4.

#include <G4LogicalVolume.hh>
#include <G4PVPlacement.hh>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "GeometryService/SHiPGeometryService.h"
#include "geometry_source.hpp"
#include "phlex/core/product_query.hpp"
#include "phlex/source.hpp"

namespace {

class GeoModelGeometrySource : public SHiP::IGeometrySource {
  std::string db_path_;
  std::vector<std::string> sensitive_vols_;
  mutable std::once_flag init_flag_;
  mutable std::unique_ptr<ship::SHiPGeometryService> service_;
  mutable G4VPhysicalVolume* world_ = nullptr;

 public:
  GeoModelGeometrySource(std::string path, std::vector<std::string> sv)
      : db_path_{std::move(path)}, sensitive_vols_{std::move(sv)} {}

  [[nodiscard]] G4VPhysicalVolume* construct() const override {
    std::call_once(init_flag_, [this]() {
      service_ = ship::SHiPGeometryService::fromFile(db_path_);
      auto* worldLV = service_->geant4WorldLogical();
      if (!worldLV)
        throw std::runtime_error(
            "GeoModelGeometrySource: GeoModel->G4 conversion failed for " +
            db_path_);

      // Wrap the logical volume in a physical volume placement (world)
      world_ = new G4PVPlacement(nullptr, G4ThreeVector(), worldLV,
                                 worldLV->GetName(), nullptr, false, 0);
    });
    return world_;
  }

  [[nodiscard]] std::vector<std::string> const& sensitiveVolumes()
      const override {
    return sensitive_vols_;
  }
};

}  // namespace

PHLEX_REGISTER_PROVIDERS(s, config) {
  using namespace phlex;

  auto db_file = config.get<std::string>("db_file");
  auto sv = config.get<std::vector<std::string>>("sensitive_volumes");

  auto source =
      std::make_shared<GeoModelGeometrySource>(db_file, std::move(sv));

  s.provide(
       "create_geometry",
       [source](data_cell_index const&)
           -> std::shared_ptr<SHiP::IGeometrySource> { return source; },
       concurrency::unlimited)
      .output_product(
          product_query{.creator = "geometry"_id, .layer = "event"_id});
}
