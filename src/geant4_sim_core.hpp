// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geant4_sim_core.hpp — Shared G4 user action classes and thread-local storage

#pragma once

#include <G4LogicalVolume.hh>
#include <G4Step.hh>
#include <G4StepPoint.hh>
#include <G4SystemOfUnits.hh>
#include <G4Track.hh>
#include <G4UserSteppingAction.hh>
#include <G4UserTrackingAction.hh>
#include <G4VProcess.hh>
#include <G4VSensitiveDetector.hh>
#include <SHiP/SimHit.hpp>
#include <SHiP/SimParticle.hpp>
#include <unordered_map>
#include <vector>

namespace SHiP::g4 {

// Thread-local storage for current event data (per G4 worker thread)
inline thread_local std::vector<SimHit> tl_hits;
inline thread_local std::vector<SimParticle> tl_particles;
inline thread_local std::unordered_map<int, std::size_t> tl_track_map;

using DetectorIdMap = std::unordered_map<G4LogicalVolume*, int>;

inline SimHit make_base_hit(G4Step const* step,
                            DetectorIdMap const& detector_ids) {
  auto* pre = step->GetPreStepPoint();
  auto pos = pre->GetPosition();
  auto mom = pre->GetMomentum();
  auto* lv = pre->GetTouchable()->GetVolume()->GetLogicalVolume();

  SimHit hit;
  auto it = detector_ids.find(lv);
  hit.detectorId = it != detector_ids.end() ? it->second : -1;
  hit.trackId = step->GetTrack()->GetTrackID();
  hit.pdgCode = step->GetTrack()->GetDefinition()->GetPDGEncoding();
  hit.position = {pos.x() / mm, pos.y() / mm, pos.z() / mm};
  hit.momentum = {mom.x() / GeV, mom.y() / GeV, mom.z() / GeV};
  hit.time = pre->GetGlobalTime() / ns;
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
    hit.energyDeposit = edep / GeV;
    hit.pathLength = step->GetStepLength() / mm;
    tl_hits.push_back(hit);
    return true;
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
             double ke_threshold_gev = 0.0)
      : G4VSensitiveDetector(name),
        detector_ids_{std::move(detector_ids)},
        ke_threshold_{ke_threshold_gev * GeV} {}

  G4bool ProcessHits(G4Step* step, G4TouchableHistory*) override {
    if (!step->IsFirstStepInVolume()) return false;

    auto* track = step->GetTrack();
    if (track->GetKineticEnergy() < ke_threshold_) return false;

    auto hit = make_base_hit(step, detector_ids_);
    hit.energyDeposit = 0;
    hit.pathLength = track->GetTrackLength() / mm;
    tl_hits.push_back(hit);
    return true;
  }

 private:
  DetectorIdMap detector_ids_;
  double ke_threshold_;
};

// Kills tracks below kinetic energy threshold.
// Matches FairShip's PreTrack() stopping behaviour.
class EnergyCutAction : public G4UserSteppingAction {
 public:
  explicit EnergyCutAction(double ke_threshold_gev)
      : ke_threshold_{ke_threshold_gev * GeV} {}

  void UserSteppingAction(const G4Step* step) override {
    auto* track = step->GetTrack();
    if (track->GetKineticEnergy() < ke_threshold_) {
      track->SetTrackStatus(fStopAndKill);
    }
  }

 private:
  double ke_threshold_;
};

class TrackingAction : public G4UserTrackingAction {
 public:
  explicit TrackingAction(double particle_ke_cut_gev = 0.0)
      : particle_ke_cut_{particle_ke_cut_gev * GeV} {}

  void PreUserTrackingAction(const G4Track* track) override {
    if (particle_ke_cut_ > 0 && track->GetParentID() != 0 &&
        track->GetKineticEnergy() < particle_ke_cut_)
      return;

    SimParticle p;
    p.trackId = track->GetTrackID();
    p.parentId = track->GetParentID();
    p.pdgCode = track->GetDefinition()->GetPDGEncoding();

    auto pos = track->GetPosition();
    auto mom = track->GetMomentum();
    p.vertex = {pos.x() / mm, pos.y() / mm, pos.z() / mm};
    p.momentum = {mom.x() / GeV, mom.y() / GeV, mom.z() / GeV};
    p.energy = track->GetKineticEnergy() / GeV;
    p.time = track->GetGlobalTime() / ns;

    auto* creator = track->GetCreatorProcess();
    p.creatorProcess = creator ? creator->GetProcessSubType() : 0;

    tl_track_map[p.trackId] = tl_particles.size();
    tl_particles.push_back(p);
  }

  void PostUserTrackingAction(const G4Track* track) override {
    auto it = tl_track_map.find(track->GetTrackID());
    if (it != tl_track_map.end()) {
      auto pos = track->GetPosition();
      tl_particles[it->second].endpoint = {pos.x() / mm, pos.y() / mm,
                                           pos.z() / mm};
    }
  }

 private:
  double particle_ke_cut_;
};

}  // namespace SHiP::g4
