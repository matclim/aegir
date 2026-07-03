// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// field_null_provider.cpp — Phlex provider plugin for workflows that don't
// need a magnetic field. Publishes an empty IFieldSource (no regions);
// ConstructSDandField() then installs no G4FieldManager and Geant4 propagates
// in vacuum throughout.

#include <memory>
#include <vector>

#include "FieldService/IFieldSource.h"
#include "phlex/source.hpp"
#include "provider_helpers.hpp"

namespace {

class NullFieldSource : public ship::IFieldSource {
  std::vector<ship::FieldRegion> empty_;

 public:
  [[nodiscard]] std::vector<ship::FieldRegion> const& regions() const override {
    return empty_;
  }
};

}  // namespace

PHLEX_REGISTER_PROVIDERS(s, config) {
  using namespace phlex;
  (void)config;

  // Publish as the interface type: consumers request
  // std::shared_ptr<ship::IFieldSource>.
  std::shared_ptr<ship::IFieldSource> source =
      std::make_shared<NullFieldSource>();

  aegir::provide_constant(s, "create_field", source, "field", "map", "job");
}
