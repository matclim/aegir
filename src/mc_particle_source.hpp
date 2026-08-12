// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// mc_particle_source.hpp — shared helper for event-generator sources
//
// Phlex 0.3.0 lets a source register its data-product providers implicitly:
// a phlex::source implements create_providers(selector), returning the
// provider bundles that satisfy the requested product. Every aegir event
// generator publishes the same products — the "mc_particles" collection
// (std::vector<SHiP::MCParticle>) and the "event_header" record
// (SHiP::EventHeader), both on the "event" layer — so the bundle
// construction is factored out here.
//
// The header carries the per-event weight. Sources that sample uniformly
// leave the generator empty and get the unweighted default (weight 1.0,
// no originating id), which keeps the output schema identical across all
// workflows; sources reading a weighted record (eventcalc_source) pass a
// generator.

#pragma once

#include <SHiP/EventHeader.hpp>
#include <SHiP/MCParticle.hpp>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "phlex/concurrency.hpp"
#include "phlex/core/product_selector.hpp"
#include "phlex/model/data_cell_index.hpp"
#include "phlex/model/products.hpp"
#include "phlex/source.hpp"

namespace aegir {

// Generator signature: produce the particles for a single data cell (event).
using mc_particle_generator =
    std::function<std::vector<SHiP::MCParticle>(phlex::data_cell_index const&)>;

// Generator signature: produce the metadata for a single data cell. May be
// empty, in which case the unweighted default is published.
using event_header_generator =
    std::function<SHiP::EventHeader(phlex::data_cell_index const&)>;

// Build the implicit-provider bundle(s) for a source emitting the
// "mc_particles" and "event_header" products, honouring the framework's
// product selector. The generators are invoked once per data cell and their
// results type-erased into Phlex products.
inline phlex::detail::provider_bundles mc_particle_provider_bundles(
    phlex::product_selector const& selector, mc_particle_generator generate,
    phlex::concurrency max_concurrency,
    event_header_generator generate_header = {}) {
  using namespace phlex::experimental;
  using namespace phlex::detail;

  provider_bundles bundles;
  std::string const layer = "event";
  std::string const stage = "CURRENT";

  product_specification particles_spec{algorithm_name::create("mc_particles"),
                                       identifier{"particles"},
                                       make_type_id<std::vector<SHiP::MCParticle>>()};

  if (selector.match(particles_spec, identifier{layer}, identifier{stage})) {
    bundles.push_back(provider_bundle{
        .provider_function =
            [generate = std::move(generate)](phlex::data_cell_index const& id)
            -> product_ptr { return product_for(generate(id)); },
        .max_concurrency = max_concurrency,
        .spec = std::move(particles_spec),
        .layer = layer,
        .stage = stage});
  }

  product_specification header_spec{algorithm_name::create("event_header"),
                                    identifier{"header"},
                                    make_type_id<SHiP::EventHeader>()};

  if (selector.match(header_spec, identifier{layer}, identifier{stage})) {
    bundles.push_back(provider_bundle{
        .provider_function =
            [generate_header = std::move(generate_header)](
                phlex::data_cell_index const& id) -> product_ptr {
              // Unweighted default: every event counts once, no provenance.
              if (!generate_header) return product_for(SHiP::EventHeader{});
              return product_for(generate_header(id));
            },
        .max_concurrency = max_concurrency,
        .spec = std::move(header_spec),
        .layer = layer,
        .stage = stage});
  }

  return bundles;
}

}  // namespace aegir
