// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// field_covfie_provider.cpp — Phlex provider plugin for covfie-backed
// magnetic field maps. Builds a ship::CovfieFieldSource from a jsonnet
// `magnets` list and publishes it as a Job-layer product.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FieldService/CovfieFieldSource.h"
#include "FieldService/IFieldSource.h"
#include "phlex/source.hpp"

PHLEX_REGISTER_PROVIDERS(s, config) {
  using namespace phlex;

  std::vector<ship::CovfieFieldSource::MagnetConfig> magnets;
  for (auto const& m :
       config.get<std::vector<phlex::configuration>>("magnets")) {
    magnets.push_back({
        m.get<std::string>("name"),
        m.get<std::string>("volume_pattern"),
        m.get<std::string>("cvf_file"),
    });
  }

  auto source = std::make_shared<ship::CovfieFieldSource>(std::move(magnets));

  s.provide(
       "create_field",
       [source](data_cell_index const&) -> std::shared_ptr<ship::IFieldSource> {
         return source;
       },
       concurrency::unlimited)
      .output_product("field", "map", "event");
}
