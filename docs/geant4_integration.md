<!--
SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Geant4 integration strategy

aegir uses a **direct worker** integration pattern where Phlex framework
threads act as Geant4 worker threads directly, with no separate G4 thread
pool or event queue. This is inspired by the
[CMSSW OscarMTProducer](https://github.com/cms-sw/cmssw/blob/master/SimG4Core/Application/src/OscarMTProducer.cc)
pattern used in CMS.

## Why direct workers?

We evaluated five integration strategies in the
[ship-phlex-sim](https://github.com/ShipSoft/ship-phlex-sim) benchmarking
repository:

1. **Synchronous single-threaded** -- `G4RunManager::BeamOn(1)` on a
   dedicated thread.
2. **Asynchronous single-threaded** -- dedicated G4 thread with an event
   queue and `std::future` for synchronisation.
3. **Synchronous multi-threaded (slots)** -- `G4TaskRunManager` with
   per-worker rendezvous slots.
4. **Asynchronous multi-threaded (queue)** -- `G4MTRunManager` on a
   dedicated thread; G4 workers pop from a concurrent queue.
5. **Direct workers** -- framework threads are G4 workers; no queue, no
   `BeamOn`.

Strategies 1--4 all involve cross-thread handoff overhead (queues, promises,
condition variables). The direct worker approach eliminates this entirely:
the thread that receives the event from the framework is the same thread
that processes it through Geant4.

In benchmarks with particle gun events on a 4-core machine, the direct
pattern matched the best queue-based strategy (sync MT slots) while being
simpler to configure and reason about. It also removes the `num_events`
configuration footgun present in the queue-based MT module, where the
configured event count had to exactly match the driver's total.

## Architecture

```
Master thread (std::thread)
  |
  +-- Owns G4MTRunManager
  +-- Runs Initialize() + RunInitialization()
  +-- Stores world volume, physics list, detector construction
  +-- Blocks until shutdown

Phlex TBB worker threads (N = concurrency)
  |
  +-- Lazy init: G4WorkerRunManagerKernel per thread
  +-- simulate() builds G4Event from MCParticles
  +-- Calls G4EventManager::ProcessOneEvent()
  +-- Collects results from thread-local storage
```

### Master initialisation

A dedicated `std::thread` creates a `G4MTRunManager` and runs the full
Geant4 initialisation sequence (geometry, physics, run initialisation). It
then stores the world physical volume, physics list, and detector
construction pointers for workers to use. The master thread blocks on a
shutdown future until the module is destroyed.

### Worker initialisation

Each Phlex thread lazily initialises on its first call to `simulate()`:

1. Assigns a unique G4 thread ID via `G4Threading::G4SetThreadId()`
2. Calls `G4WorkerThread::BuildGeometryAndPhysicsVector()`
3. Creates a `G4WorkerRunManagerKernel` and defines the world volume
4. Initialises physics and sensitive detectors for this thread
5. Sets user actions (tracking, energy cut) on the event manager

### Event processing

Instead of `G4RunManager::BeamOn()`, the module:

1. Builds a `G4Event` directly from the input `MCParticle` vector, creating
   `G4PrimaryVertex` and `G4PrimaryParticle` objects (no
   `G4VUserPrimaryGeneratorAction` involved)
2. Sets the G4 state to `G4State_GeomClosed`
3. Calls `G4EventManager::ProcessOneEvent(event)`
4. Collects hits and particles from thread-local storage

This bypasses the G4 run loop entirely, giving the framework full control
over event scheduling.

### Shutdown

The `G4MTRunManager` is intentionally leaked at shutdown. Its destructor
accesses global singletons that may already be torn down during plugin
unloading, causing crashes. This is a known Geant4 limitation.

## Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `physics_list` | string | `FTFP_BERT` | Geant4 reference physics list name |
| `concurrency` | int | `1` | Number of concurrent worker threads |
| `verbosity` | int | `0` | Geant4 run verbosity level |
| `sd_mode` | string | `scoring` | Sensitive detector mode: `scoring` or `crossing` |
| `ke_threshold` | double | `0.0` | Kinetic energy threshold for crossing SD (GeV) |
| `energy_cut` | bool | `false` | Enable stepping energy cut |
| `energy_cut_threshold` | double | `ke_threshold` | KE below which tracks are killed (GeV) |
| `particle_ke_cut` | double | `0.0` | KE below which secondary particles are not recorded (GeV) |
| `regions` | map | `{}` | Volume name pattern to production cut (mm) mapping |
| `scoring_planes` | list | `[]` | Scoring planes to add to the geometry, see [Scoring planes](#scoring-planes) |
| `export_gdml` | string | *(unset)* | Write the constructed geometry to this GDML file after initialisation. Errors if the file exists. Lets external tools (e.g. the GENIE event generator) use exactly the geometry Geant4 tracks in |
| `progress_interval` | int | `100` | Log a progress line (event count and average rate) every this many simulated events; `0` disables |

For consumers that read the exported file with ROOT's TGeo importer, run
`scripts/gdml_fix_element_names.py` on it first: ROOT confuses materials
and elements that share a name after pointer-suffix stripping (Geant4's
NIST materials routinely do — material "Iron" made of element "Iron")
and silently imports them as empty mixtures. The script renames the
colliding elements; the physics is unchanged.
`scripts/gdml_target_nuclei.py` then lists the nuclide PDG codes of the
geometry — e.g. the target list neutrino cross-section splines must
cover.

Example workflow configuration:

```jsonnet
{
  modules: {
    geant4: {
      cpp: 'geant4_module',
      physics_list: 'FTFP_BERT',
      concurrency: 4,
      sd_mode: 'crossing',
      ke_threshold: 0.5,
      energy_cut: true,
      particle_ke_cut: 1.0,
      regions: { '/SHiP/target': 50, '/SHiP/muon_shield/magn_absorb': 50 },
    },
  },
}
```

## Scoring planes

A scoring plane is a thin box or cylindrical shell that records the
particles crossing it. It is aegir's counterpart of FairShip's
`exitHadronAbsorber` and is meant for flux studies: the muons leaving the
hadron absorber, the neutrinos coming out of the target, and so on.

The planes live in a Geant4 *parallel world* (`aegir_scoring_planes`),
overlaid on whatever geometry the provider built — builtin, GDML or GeoModel
alike. Geant4 tracks every particle through both worlds at once
(`G4ParallelWorldPhysics`, added to the physics list whenever planes are
configured). A plane can therefore be any size, anywhere: it can cut through
or extend beyond the detector's volumes — a muon-shield container, a magnet,
the target — without changing their navigation or their materials, since the
parallel world carries no material of its own. Planes must not overlap one
another or extend beyond the world volume.

A plane records one `SimHit` for every track that enters it through its
surface, whatever the particle — neutrinos and other neutrals included — and
whether or not it deposits energy. It does this through its own sensitive
detector, so it works the same with `sd_mode: 'scoring'` and
`sd_mode: 'crossing'`, alongside the geometry's own sensitive volumes, and
the `sensitive_volumes` and field `volume_pattern` patterns never match the
planes. A track entering a plane twice (a cylinder crossed in and out, a
particle scattered back) is recorded each time; set `kill` to stop a track at
the first plane it reaches.

Tracking through a parallel world uses Geant4's coupled transportation and
costs some CPU time per step, so only configure the planes a run needs.

The hits are told apart from detector hits by their ids:

| `SimHit` field | Scoring-plane value |
|:---|:---|
| `detectorId` | `100` (`SHiP::g4::kScoringPlaneDetectorId`), clear of the SHiP subsystem ids and of the `sensitive_volumes` indices |
| `geometryNodeId` | the plane's `id` |
| `position`, `momentum`, `time` | at the entry point |
| `energyDeposit` | `0` |
| `pathLength` | track length from the track's origin to the end of the step |

In the output module's validation histograms, plane hits fill their own
`h_plane_hit_*` histograms (hits per event, plane id, momentum, PDG code) and
are left out of the `h_hit_*` ones, which keep describing the detector's
hits: with detector id 100 and no energy deposit, plane hits would otherwise
land in the overflow of `h_hit_detector` and pile up at zero in `h_hit_edep`.

Each entry of `scoring_planes` takes:

| Key | Type | Default | Description |
|:---|:---|:---|:---|
| `name` | string | — | Volume name; must not already exist in the geometry |
| `id` | int | position in the list | Written to `SimHit::geometryNodeId`; unique |
| `shape` | string | `box` | `box` or `tube` (cylindrical shell, axis along z) |
| `half_x`, `half_y` | double | — | Box half-widths (mm); required for a box, rejected for a tube |
| `r_min`, `r_max` | double | — | Tube inner and outer radius (mm); required for a tube, rejected for a box |
| `half_z` | double | — | Half-length along z (mm); required for both shapes |
| `position` | [x, y, z] | — | Centre in the global frame (mm) |
| `after_volume` | string | — | Place the plane downstream of this detector volume, centred transversely on its global bounding box |
| `gap` | double | `0` | With `after_volume`: distance from the volume's downstream end to the plane's upstream face (mm) |
| `ke_threshold` | double | `0` | Record only tracks entering with at least this kinetic energy (GeV) |
| `kill` | bool | `false` | Stop and kill every track the plane records |

Exactly one of `position` and `after_volume` must be given. `after_volume`
is an exact logical-volume name, and the volume must be placed exactly once
in the geometry — otherwise the plane's position would be ambiguous. Planes
are always aligned with the global axes. Unknown keys and inconsistent
settings fail the job at configuration time; a plane that overlaps another
plane or extends beyond the world (checked with
`G4VPhysicalVolume::CheckOverlaps`) fails it at initialisation.

```jsonnet
geant4: lib.geant4 {
  scoring_planes: [
    {
      // FairShip's plane 1 cm behind the hadron absorber
      name: 'exit_hadron_absorber',
      id: 1,
      half_x: 3559.0, half_y: 1699.0, half_z: 1.0,
      after_volume: '/SHiP/muon_shield/magn_absorb',
      gap: 10.0,
      kill: true,
    },
    {
      // cylindrical shell around a target centred at the origin
      name: 'around_target',
      id: 2,
      shape: 'tube',
      r_min: 80.0, r_max: 81.0, half_z: 500.0,
      position: [0.0, 0.0, 0.0],
    },
  ],
}
```

`workflows/gun_st_scoring_planes.jsonnet` places planes of each kind in the
builtin geometry, one of them cutting through the tungsten target, and
`scripts/scoring_planes.sh` (ctest `scoring_planes`) checks the recorded hits
against the geometry for neutrinos and muons.
