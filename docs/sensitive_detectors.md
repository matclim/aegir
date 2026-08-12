<!--
SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration

SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Sensitive detectors

`geant4_module` assigns one sensitive detector to every logical volume whose
name contains a pattern from the geometry provider's `sensitive_volumes` list.
The `sd_mode` key selects which one, and the three differ in *when a `SimHit`
is created* — not in which volumes are read out.

| `sd_mode` | one hit per | `energyDeposit` | typical use |
|:---|:---|:---|:---|
| `scoring` (default) | Geant4 step with `edep > 0` | that step | raw stepping truth |
| `merged` | (placement, track) | summed over the track's steps there | readout-like truth |
| `crossing` | volume entry | always `0` | acceptance, tracking efficiency |

## Choosing a mode

`crossing` answers "did this particle reach here". It records a hit when a
track first enters a volume, regardless of deposit, and can filter on kinetic
energy with `ke_threshold`. It reproduces FairShip's `exitHadronAbsorber`
behaviour. Energy is never filled, so it cannot answer calorimetry questions.

`scoring` is the raw Geant4 view: one hit per step. Faithful but verbose — a
muon crossing a single straw produces as many hits as the stepper took steps,
each with a slice of the energy. Useful when the step structure itself matters.

`merged` is what most analyses want. Every step a track takes inside one
placement collapses to one hit carrying the total deposit. Position, momentum
and time come from the first step (the entry point); `energyDeposit` and
`pathLength` are summed.

## Identifying a placement

Merging needs to tell one copy of a volume from another, and `detectorId` only
names the subsystem: it is the *index* of the matching pattern in
`sensitive_volumes`. The SHiP geometry places one straw-gas logical volume 9600
times, so `detectorId` alone cannot say which straw was hit.

`SimHit::geometryNodeId` fills that gap. It is a hash of the copy numbers and
volume pointers along the touchable path, so each placement gets a distinct
value. It is written in every mode, not just `merged`.

Two caveats. The id is stable within a run but **not** across geometry changes
or between releases — never persist it as a channel number. And being a hash
it can in principle collide; it identifies a node, it does not encode a
position in the hierarchy.

## merge_across_tracks

By default `merged` keys on `(geometryNodeId, trackId)`, so two particles
depositing in the same volume produce two hits. Setting

```jsonnet
    geant4: lib.geant4 { sd_mode: 'merged', merge_across_tracks: true },
```

widens the key to the placement alone: every particle depositing in one volume
yields a single hit. Only `energyDeposit` and `pathLength` combine
meaningfully. The rest degrade:

| field | with `merge_across_tracks` |
|:---|:---|
| `energyDeposit`, `pathLength` | summed over all contributing tracks |
| `trackId`, `pdgCode` | the largest single-track contributor |
| `position`, `momentum`, `time` | the first step to arrive in the placement |

**This discards per-particle truth.** Questions like "was this SBT hit caused
by the signal muon or by a shower stray" and truth matching for tracking
efficiency stop being answerable. The setting is off by default for that
reason.

What it buys is size. On a sample of nine 1 GeV HNL decays through the full
GeoModel geometry, hits per event dropped by factors of 1.3 to 13 (mean around
4), the largest reductions being showers where dozens of secondaries pepper
one scintillator cell.

The alternative is to leave it off and merge across tracks at digitisation
instead, where a straw's several truth hits become one `StrawTubesHit` and the
links back to the contributing particles can be kept. That also fits the
hierarchy in the data model, which combines pile-up from several collisions at
the time-frame level — above a single Geant4 event. Merging inside the
sensitive detector happens before that stage, so the information is gone by
the time it would be needed again.

Use `merge_across_tracks` when output size dominates and per-particle truth is
not needed; prefer digitisation when it is.
