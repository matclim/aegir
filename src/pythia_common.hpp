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

#include <SHiP/Units.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "record_mode.hpp"

namespace SHiP::aegir {

// Pythia8's native units: energies/momenta in GeV, positions in mm,
// production times in mm/c.
using PythiaTime = mp_units::quantity<SHiP::units::mm_per_c, double>;

// Map a 32-bit base seed into Pythia's valid Random:seed range
// [1, 900000000]. extra_streams reserves headroom for consecutive
// per-instance seeds (PythiaParallel seeds helper i with Random:seed + i).
inline int pythia_seed(std::uint32_t base, int extra_streams = 0) {
  if (extra_streams < 0 || extra_streams >= 900000000) {
    throw std::invalid_argument("pythia_seed: extra_streams " +
                                std::to_string(extra_streams) +
                                " must be in [0, 900000000)");
  }
  auto const range = static_cast<std::uint32_t>(900000000 - extra_streams);
  return static_cast<int>(base % range) + 1;
}

// Configure a fixed-target beam: beam A on a stationary target B (frameType 2,
// eB = 0). Templated so it works for both Pythia8::Pythia and PythiaParallel.
template <typename Pythia>
void configure_beams(Pythia& pythia, int idA, int idB,
                     SHiP::Energy beam_energy) {
  pythia.readString("Beams:idA = " + std::to_string(idA));
  pythia.readString("Beams:idB = " + std::to_string(idB));
  pythia.readString("Beams:frameType = 2");
  pythia.readString(
      "Beams:eA = " +
      std::to_string(beam_energy.numerical_value_in(SHiP::units::GeV)));
  pythia.readString("Beams:eB = 0.");
}

// PDG code of the nucleus (Z, A) in its ground state: 100ZZZAAA0.
inline int nucleus_pdg(int z, int a) {
  if (z < 1 || a < z || a > 999) {
    throw std::invalid_argument("nucleus_pdg: invalid nucleus Z = " +
                                std::to_string(z) +
                                ", A = " + std::to_string(a));
  }
  return 1000000000 + z * 10000 + a * 10;
}

// A beam of energy `beam_energy` on a nucleon at rest, both of mass
// `nucleon_mass` (GeV): the nucleon-nucleon centre-of-mass energy sqrt(s_NN)
// and the velocity of that frame in the lab, along the beam. Heavy-ion models
// (Angantyr) take the energy per nucleon pair; the event is generated in this
// frame and boosted to the lab.
struct NucleonPairFrame {
  double ecm;   // GeV
  double beta;  // of the centre-of-mass frame in the lab, along +z
};

inline NucleonPairFrame nucleon_pair_frame(SHiP::Energy beam_energy,
                                           double nucleon_mass) {
  double const e = beam_energy.numerical_value_in(SHiP::units::GeV);
  double const m = nucleon_mass;
  if (!(m > 0) || !(e > m)) {
    throw std::invalid_argument(
        "nucleon_pair_frame: the beam energy must exceed the nucleon mass");
  }
  double const p = std::sqrt(e * e - m * m);
  return {.ecm = std::sqrt(2 * m * m + 2 * e * m), .beta = p / (e + m)};
}

// Configure a proton beam on a nucleus at rest for Pythia's heavy-ion model
// (Angantyr): generated in the nucleon-nucleon centre-of-mass frame
// (frameType 1), which the caller boosts to the lab with the returned beta.
// Pythia's fixed-target frame (frameType 2, eB = 0) does not give Angantyr a
// valid nucleon-nucleon system: its sub-collision set-up fails. A nucleus
// unknown to Pythia (such as 96Mo) makes its event check fail.
template <typename Pythia>
NucleonPairFrame configure_nucleus_target(Pythia& pythia, int target_z,
                                          int target_a,
                                          SHiP::Energy beam_energy) {
  auto const frame =
      nucleon_pair_frame(beam_energy, pythia.particleData.m0(2212));
  int const id = nucleus_pdg(target_z, target_a);
  // Pythia knows only a few nuclei (e.g. 208Pb, 197Au, 63Cu); any other
  // target is defined here, with the mass A u as Pythia's own nuclei
  // (u = 0.931494 GeV), so that its event record passes Pythia's checks.
  if (!pythia.particleData.isParticle(id)) {
    std::string const name =
        "Z" + std::to_string(target_z) + "A" + std::to_string(target_a);
    pythia.readString(std::to_string(id) + ":all = " + name + " " + name +
                      "bar 0 " + std::to_string(3 * target_z) + " 0 " +
                      std::to_string(0.931494 * target_a));
  }
  pythia.readString("Beams:idA = 2212");
  pythia.readString("Beams:idB = " + std::to_string(id));
  pythia.readString("Beams:frameType = 1");
  pythia.readString("Beams:eCM = " + std::to_string(frame.ecm));
  pythia.readString("HeavyIon:mode = 1");
  return frame;
}

// Make long-lived particles (tau0 above threshold) stable so a downstream
// simulation (e.g. Geant4) handles their decay. Guards against null
// particleData entries.
template <typename Pythia>
void stabilise_long_lived(Pythia& pythia, PythiaTime tau0_threshold) {
  double const threshold =
      tau0_threshold.numerical_value_in(SHiP::units::mm_per_c);
  for (auto it = pythia.particleData.begin(); it != pythia.particleData.end();
       ++it) {
    auto const& entry = it->second;  // ParticleDataEntryPtr (shared_ptr-like)
    if (entry && entry->tau0() > threshold) {
      entry->setMayDecay(false);
    }
  }
}

// Advance the generator to its next event, retrying transient failures.
// Pythia8::next() can occasionally reject a trial event; persistent failure
// is a hard error rather than a silently-empty event, so it cannot leak
// empty entries into the output (same convention as Pythia8MTSource
// exhaustion).
template <typename Pythia>
void next_event(Pythia& pythia, std::string_view source_name,
                int max_attempts = 10) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (pythia.next()) {
      return;
    }
  }
  throw std::runtime_error(std::string(source_name) +
                           ": Pythia8 event generation failed " +
                           std::to_string(max_attempts) + " times in a row");
}

// Extract particles from a Pythia event record into a vector of MCParticle
// (any type exposing pdg_code/vertex/momentum/energy/time/mother_id/status).
// vertex z is shifted by z_offset.
//
// In final_state mode only undecayed particles are emitted; in full mode the
// whole record is, so beam particles, decayed mothers and intermediate
// entries appear too. The emitted `status` is the HepMC status
// (Pythia8's statusHepMC(); see hepmc_status.hpp for the code set), so a
// consumer can tell the two apart — geant4_module tracks exactly status 1.
// Pythia's entry 0, the "system" pseudo-particle, is never emitted.
//
// `mothers` is the complete mother list, taken from Pythia's motherList().
// That accessor is derived from mother1, mother2 *and* the native status, so
// it already resolves the six mother1/mother2 combinations — a carbon copy, a
// single mother, an inclusive range of string-fragmentation mothers, two
// genuinely distinct mothers — into one uniform list. It is empty for the
// beam particles, whose history Pythia does not record.
//
// Indices are remapped from the Pythia record to the returned vector.
// Mothers that were not themselves written out are dropped rather than
// recorded as -1, because the data model reserves -1 for mother_id alone and
// requires every element of `mothers` to be a valid index. In final_state
// mode most mothers are dropped, since mothers of final-state particles
// generally are not final state themselves; in full mode the map degenerates
// to "record index minus one" and the whole chain survives.
//
// mother_id is then the first surviving mother, or -1 when none survived,
// which keeps it equal to mothers.front() as the data model requires.
template <typename MCParticle>
std::vector<MCParticle> extract_particles(
    Pythia8::Event const& event, SHiP::Length z_offset = SHiP::Length::zero(),
    record_mode mode = record_mode::final_state) {
  namespace su = SHiP::units;
  std::vector<MCParticle> particles;
  particles.reserve(event.size());

  // Pythia-record index -> output index for written particles. Entry 0 is
  // never written, so out_index[0] stays -1 and mother1() == 0 ("no mother"
  // in Pythia) maps onto MCParticle's -1 without a special case.
  std::vector<int> out_index(static_cast<std::size_t>(event.size()), -1);

  for (int i = 1; i < event.size(); ++i) {
    auto const& p = event[i];
    if (mode == record_mode::final_state && !p.isFinal()) {
      continue;
    }

    out_index[static_cast<std::size_t>(i)] = static_cast<int>(particles.size());

    MCParticle mc;
    mc.pdg_code = p.id();
    // Each Pythia accessor acquires its unit on the read line; SHiP::raw is
    // the single bitwise unwrap into the storage units. The SHiP::view
    // setters are unavailable here because MCParticle is a template parameter
    // (the benchmark instantiates a local struct without the data model).
    mc.vertex = SHiP::raw(SHiP::Vec3<SHiP::Length>{
        p.xProd() * su::mm, p.yProd() * su::mm, p.zProd() * su::mm + z_offset});
    mc.momentum = SHiP::raw(SHiP::Vec3<SHiP::Momentum>{p.px() * su::GeV_per_c,
                                                       p.py() * su::GeV_per_c,
                                                       p.pz() * su::GeV_per_c});
    mc.energy = SHiP::raw(p.e() * su::GeV);
    // mm/c -> ns via the exact definition of c (no hand-typed constant).
    mc.time = (p.tProd() * su::mm_per_c).numerical_value_in(su::ns);
    mc.status = p.statusHepMC();
    // Record indices for now; remapped below, once out_index is complete.
    for (int const m : p.motherList()) {
      mc.mothers.push_back(m);
    }
    particles.push_back(mc);
  }

  for (auto& mc : particles) {
    std::vector<std::int32_t> mapped;
    mapped.reserve(mc.mothers.size());
    for (int const m : mc.mothers) {
      if (m < 0 || std::cmp_greater_equal(m, out_index.size())) {
        continue;
      }
      if (int const out = out_index[static_cast<std::size_t>(m)]; out >= 0) {
        mapped.push_back(out);
      }
    }
    mc.mothers = std::move(mapped);
    mc.mother_id = mc.mothers.empty() ? -1 : mc.mothers.front();
  }
  return particles;
}

}  // namespace SHiP::aegir
