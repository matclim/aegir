// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// pythia_common.hpp — helpers shared by the Pythia8-based generators
//
// Header-only and free of Phlex/data-model dependencies so the standalone
// benchmark can share the same code. extract_particles() is templated on the
// output particle type (any struct exposing the MCParticle fields), letting
// the plugins emit SHiP::MCParticle while the benchmark keeps its local
// stand-in.

#pragma once

#include <Pythia8/Pythia.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace aegir {

// Configure a fixed-target beam: beam A on a stationary target B (frameType 2,
// eB = 0). Templated so it works for both Pythia8::Pythia and PythiaParallel.
template <typename Pythia>
void configure_beams(Pythia& pythia, int idA, int idB, double beam_energy) {
  pythia.readString("Beams:idA = " + std::to_string(idA));
  pythia.readString("Beams:idB = " + std::to_string(idB));
  pythia.readString("Beams:frameType = 2");
  pythia.readString("Beams:eA = " + std::to_string(beam_energy));
  pythia.readString("Beams:eB = 0.");
}

// Make long-lived particles (tau0 above threshold, in mm/c) stable so a
// downstream simulation (e.g. Geant4) handles their decay. Guards against null
// particleData entries.
template <typename Pythia>
void stabilise_long_lived(Pythia& pythia, double tau0_threshold) {
  for (auto it = pythia.particleData.begin(); it != pythia.particleData.end();
       ++it) {
    auto& entry = it->second;  // ParticleDataEntryPtr (shared_ptr-like)
    if (entry && entry->tau0() > tau0_threshold) entry->setMayDecay(false);
  }
}

// Advance the generator to its next event, retrying transient failures.
// Pythia8::next() can occasionally reject a trial event; persistent failure
// is a hard error rather than a silently-empty event, so it cannot leak
// empty entries into the output (same convention as Pythia8MTSource
// exhaustion).
template <typename Pythia>
void next_event(Pythia& pythia, char const* source_name,
                int max_attempts = 10) {
  for (int attempt = 0; attempt < max_attempts; ++attempt)
    if (pythia.next()) return;
  throw std::runtime_error(std::string(source_name) +
                           ": Pythia8 event generation failed " +
                           std::to_string(max_attempts) + " times in a row");
}

// Extract final-state particles from a Pythia event record into a vector of
// MCParticle (any type exposing pdgCode/vertex/momentum/energy/time/motherId/
// status). vertex z is shifted by z_offset (mm).
//
// motherId is remapped from the full Pythia-record index to the index within
// the returned vector, or -1 when the mother was not itself written out — the
// common case, since only final-state particles are kept and their mothers
// generally are not. This makes motherId a valid index into the emitted
// collection rather than a dangling reference into the discarded record.
template <typename MCParticle>
std::vector<MCParticle> extract_particles(Pythia8::Event const& event,
                                          double z_offset = 0.0) {
  std::vector<MCParticle> particles;
  particles.reserve(event.size());

  // Pythia-record index -> output index for written (final-state) particles.
  std::vector<int> out_index(static_cast<std::size_t>(event.size()), -1);

  for (int i = 0; i < event.size(); ++i) {
    auto const& p = event[i];
    if (!p.isFinal()) continue;

    out_index[static_cast<std::size_t>(i)] = static_cast<int>(particles.size());

    MCParticle mc;
    mc.pdgCode = p.id();
    mc.vertex = {p.xProd(), p.yProd(), p.zProd() + z_offset};  // mm
    mc.momentum = {p.px(), p.py(), p.pz()};                    // GeV
    mc.energy = p.e();
    mc.time = p.tProd() / 299.792458;  // mm/c -> ns
    mc.motherId = p.mother1();         // record index, remapped below
    mc.status = p.statusHepMC();
    particles.push_back(mc);
  }

  for (auto& mc : particles) {
    int m = mc.motherId;
    mc.motherId = (m >= 0 && m < static_cast<int>(out_index.size()))
                      ? out_index[static_cast<std::size_t>(m)]
                      : -1;
  }
  return particles;
}

}  // namespace aegir
