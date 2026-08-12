// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geant4_sim_core.hpp — Shared G4 user action classes and thread-local storage

#pragma once

#include <G4LogicalVolume.hh>
#include <G4Step.hh>
#include <G4StepPoint.hh>
#include <G4Track.hh>
#include <G4UserSteppingAction.hh>
#include <G4UserTrackingAction.hh>
#include <G4VProcess.hh>
#include <G4VSensitiveDetector.hh>
#include <G4VTouchable.hh>
#include <SHiP/QuantityView.hpp>
#include <SHiP/SimHit.hpp>
#include <SHiP/SimParticle.hpp>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "units/clhep_bridge.hpp"

namespace SHiP::g4 {

// Thread-local storage for current event data (per G4 worker thread)
inline thread_local std::vector<SimHit> tl_hits;
inline thread_local std::vector<SimParticle> tl_particles;
inline thread_local std::unordered_map<int, std::size_t> tl_track_map;

using DetectorIdMap = std::unordered_map<G4LogicalVolume*, int>;

// Stable identifier for one placement — a node of the touchable history.
// detectorId only names the subsystem, and a single logical volume can be
// placed thousands of times (9600 copies of /SHiP/trackers/straw_gas share one
// G4LogicalVolume), so the copy numbers along the touchable path are what
// distinguish one straw from another. FNV-1a over (copy number, volume) at
// every depth, folded to 31 bits because SimHit::geometryNodeId is int32 and a
// negative id would read as a sentinel.
//
// Stable within a run but not across geometry changes, and being a hash it can
// in principle collide: it identifies a node, it does not encode a position in
// the hierarchy. Never persist it as a channel number.
inline std::int32_t geometry_node_id(G4VTouchable const* touchable) {
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
  auto mix = [&h](std::uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;  // FNV-1a prime
  };
  int const depth = touchable->GetHistoryDepth();
  for (int d = 0; d <= depth; ++d) {
    mix(static_cast<std::uint64_t>(touchable->GetCopyNumber(d)));
    mix(static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(touchable->GetVolume(d))));
  }
  return static_cast<std::int32_t>((h ^ (h >> 32)) & 0x7fffffff);
}

inline SimHit make_base_hit(G4Step const* step,
                            DetectorIdMap const& detector_ids) {
  auto* pre = step->GetPreStepPoint();
  auto pos = pre->GetPosition();
  auto mom = pre->GetMomentum();
  auto* lv = pre->GetTouchable()->GetVolume()->GetLogicalVolume();

  SimHit hit;
  auto it = detector_ids.find(lv);
  hit.detectorId = it != detector_ids.end() ? it->second : -1;
  hit.geometryNodeId = geometry_node_id(pre->GetTouchable());
  hit.trackId = step->GetTrack()->GetTrackID();
  hit.pdgCode = step->GetTrack()->GetDefinition()->GetPDGEncoding();
  ship::view::setPosition(hit, aegir::clhep::position(pos));
  ship::view::setMomentum(hit, aegir::clhep::momentum(mom));
  ship::view::setTime(hit, aegir::clhep::time(pre->GetGlobalTime()));
  return hit;
}

class ScoringSD : public G4VSensitiveDetector {
 public:
  ScoringSD(G4String const& name, DetectorIdMap detector_ids)
      : G4VSensitiveDetector(name), detector_ids_{std::move(detector_ids)} {}

  G4bool ProcessHits(G4Step* step, G4TouchableHistory*) override {
    double edep = step->GetTotalEnergyDeposit();
    if (edep <= 0) return false;

    auto hit = make_base_hit(step, detector_ids_);
    ship::view::setEnergyDeposit(hit, aegir::clhep::energy(edep));
    ship::view::setPathLength(hit, aegir::clhep::length(step->GetStepLength()));
    tl_hits.push_back(hit);
    return true;
  }

 private:
  DetectorIdMap detector_ids_;
};

// One open accumulator per (placement, track) while an event is in flight.
// Namespace-scope thread_local for the same reason as tl_hits: G4 worker
// threads must not share it.
struct OpenHit {
  SimHit hit;       // taken from the first contributing step
  double edep = 0;  // Geant4 internal units, summed
  double path = 0;  // Geant4 internal units, summed
};
inline thread_local std::unordered_map<std::uint64_t, OpenHit> tl_open_hits;

// Like ScoringSD, but merges every step a track takes inside one placement
// into a single SimHit, emitted at end of event. This is what readout-like
// output wants: a track crossing a straw produces one hit carrying the total
// deposit, not one hit per Geant4 step.
//
// Position, momentum and time come from the first step (the entry point);
// energyDeposit and pathLength are summed. A track that leaves and re-enters
// the same placement merges into the same hit, since the key has no visit
// counter. See docs/sensitive_detectors.md.
class MergedScoringSD : public G4VSensitiveDetector {
 public:
  MergedScoringSD(G4String const& name, DetectorIdMap detector_ids)
      : G4VSensitiveDetector(name), detector_ids_{std::move(detector_ids)} {}

  void Initialize(G4HCofThisEvent*) override { tl_open_hits.clear(); }

  G4bool ProcessHits(G4Step* step, G4TouchableHistory*) override {
    double const edep = step->GetTotalEnergyDeposit();
    if (edep <= 0) return false;

    auto* pre = step->GetPreStepPoint();
    auto const node = geometry_node_id(pre->GetTouchable());
    auto const track = step->GetTrack()->GetTrackID();
    auto const key =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(node)) << 32) |
        static_cast<std::uint32_t>(track);

    auto [it, inserted] = tl_open_hits.try_emplace(key);
    if (inserted) it->second.hit = make_base_hit(step, detector_ids_);
    it->second.edep += edep;
    it->second.path += step->GetStepLength();
    return true;
  }

  void EndOfEvent(G4HCofThisEvent*) override {
    auto const first = tl_hits.size();
    for (auto& [key, open] : tl_open_hits) {
      ship::view::setEnergyDeposit(open.hit, aegir::clhep::energy(open.edep));
      ship::view::setPathLength(open.hit, aegir::clhep::length(open.path));
      tl_hits.push_back(open.hit);
    }
    // unordered_map iteration order is unspecified and varies with insertion
    // history, so sort the batch: the determinism test compares output across
    // runs and would otherwise fail spuriously.
    std::sort(tl_hits.begin() + static_cast<std::ptrdiff_t>(first),
              tl_hits.end(), [](SimHit const& a, SimHit const& b) {
                return std::pair{a.trackId, a.geometryNodeId} <
                       std::pair{b.trackId, b.geometryNodeId};
              });
    tl_open_hits.clear();
  }

 private:
  DetectorIdMap detector_ids_;
};

// Records a SimHit when a track first enters the volume, regardless of
// energy deposit. Optionally filters on kinetic energy threshold.
// Matches FairShip's exitHadronAbsorber scoring behaviour.
class CrossingSD : public G4VSensitiveDetector {
 public:
  CrossingSD(G4String const& name, DetectorIdMap detector_ids,
             ship::Energy ke_threshold = ship::Energy::zero())
      : G4VSensitiveDetector(name),
        detector_ids_{std::move(detector_ids)},
        ke_threshold_{aegir::clhep::g4(ke_threshold)} {}

  G4bool ProcessHits(G4Step* step, G4TouchableHistory*) override {
    if (!step->IsFirstStepInVolume()) return false;

    auto* track = step->GetTrack();
    if (track->GetKineticEnergy() < ke_threshold_) return false;

    auto hit = make_base_hit(step, detector_ids_);
    hit.energyDeposit = 0;
    ship::view::setPathLength(hit,
                              aegir::clhep::length(track->GetTrackLength()));
    tl_hits.push_back(hit);
    return true;
  }

 private:
  DetectorIdMap detector_ids_;
  double ke_threshold_;  // Geant4 internal units
};

// Kills tracks below kinetic energy threshold.
// Matches FairShip's PreTrack() stopping behaviour.
class EnergyCutAction : public G4UserSteppingAction {
 public:
  explicit EnergyCutAction(ship::Energy ke_threshold)
      : ke_threshold_{aegir::clhep::g4(ke_threshold)} {}

  void UserSteppingAction(const G4Step* step) override {
    auto* track = step->GetTrack();
    if (track->GetKineticEnergy() < ke_threshold_) {
      track->SetTrackStatus(fStopAndKill);
    }
  }

 private:
  double ke_threshold_;  // Geant4 internal units
};

class TrackingAction : public G4UserTrackingAction {
 public:
  explicit TrackingAction(ship::Energy particle_ke_cut = ship::Energy::zero())
      : particle_ke_cut_{aegir::clhep::g4(particle_ke_cut)} {}

  void PreUserTrackingAction(const G4Track* track) override {
    if (particle_ke_cut_ > 0 && track->GetParentID() != 0 &&
        track->GetKineticEnergy() < particle_ke_cut_)
      return;

    SimParticle p;
    p.trackId = track->GetTrackID();
    p.parentId = track->GetParentID();
    p.pdgCode = track->GetDefinition()->GetPDGEncoding();

    ship::view::setVertex(p, aegir::clhep::position(track->GetPosition()));
    ship::view::setMomentum(p, aegir::clhep::momentum(track->GetMomentum()));
    ship::view::setEnergy(p, aegir::clhep::energy(track->GetKineticEnergy()));
    ship::view::setTime(p, aegir::clhep::time(track->GetGlobalTime()));

    auto* creator = track->GetCreatorProcess();
    p.creatorProcess = creator ? creator->GetProcessSubType() : 0;

    tl_track_map[p.trackId] = tl_particles.size();
    tl_particles.push_back(p);
  }

  void PostUserTrackingAction(const G4Track* track) override {
    auto it = tl_track_map.find(track->GetTrackID());
    if (it != tl_track_map.end()) {
      ship::view::setEndpoint(tl_particles[it->second],
                              aegir::clhep::position(track->GetPosition()));
    }
  }

 private:
  double particle_ke_cut_;  // Geant4 internal units
};

}  // namespace SHiP::g4
