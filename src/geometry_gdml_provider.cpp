// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geometry_gdml_provider.cpp — Phlex provider plugin for GDML geometry
//
// Reads a GDML file and provides the parsed geometry as a Job-layer product.

#include <G4GDMLParser.hh>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "geometry_source.hpp"
#include "phlex/source.hpp"
#include "provider_helpers.hpp"

namespace {

class GDMLGeometrySource : public SHiP::IGeometrySource {
  std::string gdml_path_;
  std::vector<std::string> sensitive_vols_;
  mutable std::once_flag init_flag_;
  mutable G4VPhysicalVolume* world_ = nullptr;

 public:
  GDMLGeometrySource(std::string path, std::vector<std::string> sv)
      : gdml_path_{std::move(path)}, sensitive_vols_{std::move(sv)} {}

  [[nodiscard]] G4VPhysicalVolume* construct() const override {
    std::call_once(init_flag_, [this]() {
      G4GDMLParser parser;
      parser.Read(gdml_path_, /*validate=*/false);
      world_ = parser.GetWorldVolume();
      if (!world_)
        throw std::runtime_error("GDMLGeometrySource: no world volume in " +
                                 gdml_path_);
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

  auto gdml_file = config.get<std::string>("gdml_file");
  auto sv = config.get<std::vector<std::string>>("sensitive_volumes");

  auto source = std::make_shared<GDMLGeometrySource>(gdml_file, std::move(sv));

  aegir::provide_constant(s, "create_geometry", source, "geometry", "detector",
                          "event");
}
