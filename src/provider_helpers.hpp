// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// provider_helpers.hpp — shared registration for constant data-product
// providers (geometry, field). These sources are constant across all events,
// so the provider hands out the same shared instance for every data cell.

#pragma once

#include <memory>
#include <utility>

#include "phlex/concurrency.hpp"
#include "phlex/model/data_cell_index.hpp"

namespace aegir {

// Register a provider that returns a single shared instance for every data
// cell. Templated on the registrar and source type so it serves both the
// geometry (SHiP::IGeometrySource) and field (ship::IFieldSource) providers.
template <typename Registrar, typename Source>
void provide_constant(Registrar& s, char const* algorithm,
                      std::shared_ptr<Source> source, char const* creator,
                      char const* suffix, char const* layer) {
  s.provide(
       algorithm,
       [source = std::move(source)](phlex::data_cell_index const&)
           -> std::shared_ptr<Source> { return source; },
       phlex::concurrency::unlimited)
      .output_product(creator, suffix, layer);
}

}  // namespace aegir
