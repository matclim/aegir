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

1. Builds a `G4Event` directly from the input `MCParticle` vector — keeping
   only final-state entries, see [Primary selection](#primary-selection) —
   creating `G4PrimaryVertex` and `G4PrimaryParticle` objects (no
   `G4VUserPrimaryGeneratorAction` involved)
2. Sets the G4 state to `G4State_GeomClosed`
3. Calls `G4EventManager::ProcessOneEvent(event)`
4. Collects hits and particles from thread-local storage

This bypasses the G4 run loop entirely, giving the framework full control
over event scheduling.

### Primary selection

`SHiP::MCParticle::status` carries the **HepMC status code** — the same
convention Pythia8's `Particle::statusHepMC()` produces and EDM4hep uses for
its `generatorStatus`:

| Code | Meaning | Tracked? |
|------|---------|----------|
| 0 | Empty entry, no meaningful information | no |
| 1 | Final state — not decayed *by the generator*; may still be unstable | **yes** |
| 2 | Decayed Standard Model hadron, tau or muon | no |
| 3 | Documentation entry | no |
| 4 | Incoming beam particle | no |
| 11-200 | Intermediate entry, generator-dependent classification | no |
| 201+ | Left to the simulation. aegir writes 201 for the ancestry of a replayed particle, including the particle itself where it was produced (see [Replaying a plane's particles](#replaying-a-planes-particles)) | no |

Only status-1 entries become primaries. Note that "final state" does not mean
stable: a final-state `K_S` is status 1 and is expected to decay in the
detector — that is exactly the work being handed over. Everything else is
either the generator's own bookkeeping or a particle the generator has already
decayed, and tracking it alongside the daughters it already produced would
double-count the event.

The predicate lives in `src/hepmc_status.hpp`, next to the code set. Geant4's
own HepMC example uses the same `status == 1` test.

Skipped primaries are counted and reported once per event in an aggregated
warning; they remain in the output `mc_particles` product, so the record is
not lost. Entries with status 201, the history aegir writes, are left out of
the warning, since a replayed event always carries them.

This filter is a no-op for a generator running in the default
`record: 'final_state'` mode, where every emitted entry is status 1. It
matters under `record: 'full'` — see [generator_record.md](generator_record.md).

Without it the failure mode is *silent and partial* rather than loud: Geant4
declines to track short-lived definitions (quarks, gluons, diquarks, strings)
on its own, but happily tracks decayed hadrons and beam particles. Setting
`track_all_primaries` on a full record demonstrates this — the primary count
rises to exactly the number of status 1, 2 and 4 entries.

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
| `keep_particles` | string | `all` | Which `SimParticle`s are written. `all` writes every recorded particle. `hit_ancestry` writes only the particles that left a hit, plus all their ancestors, as FairShip's stack does, so every `parent_id` in the output points to a written particle. `hit_ancestry` needs `particle_ke_cut` at 0, because a skipped ancestor would break the chain |
| `track_all_primaries` | bool | `false` | Hand every input `MCParticle` to Geant4 regardless of its generator status. By default only HepMC status-1 (final-state) entries are tracked; set this only for inputs whose `status` does not follow the HepMC convention |
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

### Sensitive volumes

The GDML and GeoModel geometry providers take a `sensitive_volumes` list.
Every logical volume whose name contains one of its patterns gets the
sensitive detector chosen by `sd_mode`. If a volume matches more than one
entry, the first one wins. The `detector_id` of each hit says which SHiP
subsystem it belongs to, using the `SHiP::detector_id` values of the data
model.

An entry can be a bare pattern, and aegir then looks up its detector:

| Pattern | `detector_id` |
|:---|:---|
| `UpstreamTagger`, `upstream_tagger` | `0` (UpstreamTagger) |
| `SurroundTagger` | `1` (SurroundTagger) |
| `StrawTubes`, `trackers` | `2` (StrawTubes) |
| `Calorimeter`, `calorimeter` | `3` (Calorimeter) |
| `TimingDetector`, `timing_detector` | `4` (TimingDetector) |
| `Scoring`, `ScoringPlane` | `99`, the toy geometries' test planes, which belong to no subsystem |

For any other pattern, name the detector yourself with
`{ pattern: ..., detector: ... }`. `detector` is one of `UpstreamTagger`,
`SurroundTagger`, `StrawTubes`, `Calorimeter`, `TimingDetector` or
`test_fixture` (id 99). Both kinds of entry can go in one list, and
several entries can share a detector:

```jsonnet
geometry: {
  cpp: 'geometry_geomodel_provider',
  db_file: 'ship_geometry.db',
  sensitive_volumes: [
    'upstream_tagger',
    { pattern: 'straw_gas', detector: 'StrawTubes' },
    { pattern: 'timing_detector/bar', detector: 'TimingDetector' },
  ],
},
```

An entry written out in full can also take `copy_number_depth`. The hits in
its volumes then carry, as `geometry_node_id`, the copy number of the volume
that many levels above the one the hit is in: 0 for the volume itself, 1 for
its mother, and so on. A GDML geometry keeps the copy numbers of its
`physvol`s, so this tells its channels apart, such as the tubes of a
drift-tube layer that all share one logical volume. A hit in a volume that
sits less deep than the depth asked for gets -1. Without the key,
`geometry_node_id` stays 0.

```jsonnet
sensitive_volumes: [
  { pattern: 'gas', detector: 'StrawTubes', copy_number_depth: 0 },
],
```

A pattern aegir doesn't know, an unknown detector name, an unknown key or a
negative `copy_number_depth` stops the job while the configuration is read, before Geant4 starts. The
builtin geometry always uses `Scoring`, with id 99.

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
the target — and the parallel world carries no material of its own, so the
detector's materials apply everywhere. Planes must not overlap one another
(touching is fine) or extend beyond the world volume.

A plane records one `SimHit` for every track that enters it through its
surface, whatever the particle — neutrinos and other neutrals included — and
whether or not it deposits energy. It does this through its own sensitive
detector, so it works the same with `sd_mode: 'scoring'` and
`sd_mode: 'crossing'`, alongside the geometry's own sensitive volumes, and
the `sensitive_volumes` and field `volume_pattern` patterns never match the
planes. A track entering a plane twice (a cylinder crossed in and out, a
particle scattered back) is recorded each time; set `kill` to stop a track at
the first plane it reaches.

Planes do change how the detector geometry is stepped through. With a
parallel world registered, Geant4 uses coupled transportation, and steps in
the detector geometry also end at every plane face. Hence:

- `sd_mode: 'scoring'` writes one hit per step, so a plane cutting through a
  sensitive volume splits some of its hits in two; the total energy
  deposit is unchanged.
- `sd_mode: 'crossing'` is unaffected, since its first-step-in-volume flag
  only follows the detector's own boundaries.
- Runs with and without planes are not identical event by event, even with
  the same seed.
- Each step costs some extra CPU time, so only configure the planes a run
  needs.

The hits are told apart from detector hits by their ids:

| `SimHit` field | Scoring-plane value |
|:---|:---|
| `detector_id` | `100` (`SHiP::aegir::g4::kScoringPlaneDetectorId`), clear of the SHiP subsystem ids (0–4) and of the test planes' 99 (see [Sensitive volumes](#sensitive-volumes)) |
| `geometry_node_id` | the plane's `id` |
| `position`, `momentum`, `time` | at the entry point |
| `energy_deposit` | `0` |
| `path_length` | track length from the track's origin to the entry point. A `sd_mode: 'crossing'` hit gives it at the end of its first step in the volume instead, so the two don't mean quite the same thing |

In the output module's validation histograms, plane hits fill their own
`h_plane_hit_*` histograms (hits per event, plane id, momentum, PDG code) and
are left out of the `h_hit_*` ones, which keep describing the detector's
hits: with detector id 100 and no energy deposit, plane hits would otherwise
land in the overflow of `h_hit_detector` and pile up at zero in `h_hit_edep`.
`h_plane_hit_id` covers plane ids 0–999; other ids land in its underflow or
overflow bin. `h_plane_hit_pdg` covers PDG codes from −5999 to 5999, so
hyperons and charm and beauty baryons are included, plus one bin at ±6000
that collects all nuclei and ions.

Each entry of `scoring_planes` takes:

| Key | Type | Default | Description |
|:---|:---|:---|:---|
| `name` | string | — | Volume name; must not already exist in the geometry |
| `id` | int | — | Written to `SimHit::geometry_node_id`, telling the planes' hits apart; required and unique |
| `shape` | string | `box` | `box` or `tube` (cylindrical shell, axis along z) |
| `half_x`, `half_y` | double | — | Box half-widths (mm); required for a box, rejected for a tube |
| `r_min`, `r_max` | double | — | Tube inner and outer radius (mm); required for a tube, rejected for a box |
| `half_z` | double | — | Half-length along z (mm); required for both shapes |
| `position` | [x, y, z] | — | Centre in the global frame (mm) |
| `after_volume` | string | — | Place the plane downstream of this detector volume, centred transversely on its global bounding box |
| `gap` | double | `0` | With `after_volume`: distance from the volume's downstream end to the plane's upstream face (mm) |
| `ke_threshold` | double | `0` | Record only tracks entering with at least this kinetic energy (GeV). With `kill`, tracks below it are stopped too, unrecorded |
| `kill` | bool | `false` | Stop and kill every track that steps into the plane, as FairShip's `exitHadronAbsorber` does: recorded or not, including secondaries born inside the plane |
| `record_particles` | bool | `false` | Also write the tracks this plane records going downstream as `plane_particles`, for a later run to replay; see [Replaying a plane's particles](#replaying-a-planes-particles). Needs `kill`, and only one plane may set it |

Exactly one of `position` and `after_volume` must be given. `after_volume`
is an exact logical-volume name, and the volume must be placed exactly once
in the geometry — otherwise the plane's position would be ambiguous. Planes
are always aligned with the global axes. Unknown keys and inconsistent
settings fail the job at configuration time. A plane that overlaps another
plane or extends beyond the world fails it at initialisation, before any
plane is built. That test is exact for these axis-aligned shapes, so even a
small corner overlap between thin planes is caught.

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

### Fixed-target production

`workflows/fixed_target_production.jsonnet` is the counterpart of FairShip's
`run_fixedTarget.py`. It generates Pythia8 p-W interactions in the GeoModel
target and tracks them through the target and the hadron absorber. The
plane 1 cm behind the absorber (`lib.exit_hadron_absorber_plane`, the first
plane in the example above) records and stops every particle reaching it
above the energy cut `ecut`. Everything below `ecut` is killed wherever it
is. Only the particles reaching the plane and their ancestors are written
(`keep_particles: 'hit_ancestry'`), and events without a plane hit are
dropped, as in FairShip. Like FairShip's default, the run has no magnetic
field in the absorber.

```bash
pixi run phlex -c <(jsonnet --tla-code seed=42 --tla-code ecut=0.5 \
  --tla-code events=10000 workflows/fixed_target_production.jsonnet)
```

It needs the GeoModel geometry DB (`pixi run build_geometry`). The
`scoring_planes` test only checks that the workflow evaluates to the
intended settings.

The plane also has `record_particles` set, so the output holds the
`plane_particles` that `workflows/plane_replay.jsonnet` simulates further.

### Replaying a plane's particles

A staged simulation splits a run at a plane. The first stage, such as the
fixed-target production above, runs up to the plane. The second stage takes
the particles that reached it and simulates them through the rest of the
detector, as many times or with as many settings as needed, without
regenerating the target interactions.

The second stage starts each saved particle where it entered the plane, with
the position, momentum and time it had there. Starting it from where it was
produced instead would simulate its path to the plane a second time, with new
random numbers. The first stage only saved the particles whose path happened
to reach the plane, so many would not reach it again: in a test with
secondaries of a pion shower in 1 m of tungsten, only about 30% did, and the
replayed flux at the plane was too low at every momentum.

A plane with `record_particles` marks every track it records while the track
moves downstream (positive z momentum). Tracks entering it from downstream
are still recorded as hits but not marked. At the end of the event the marked
tracks are written to the `plane_particles` field of the output, as
`SHiP::MCParticle`s:

- Each marked track appears once with status 1, with its position,
  momentum, total energy and time where it entered the plane, the same as its
  hit in `sim_hits`.
- Its full ancestry follows with status 201: first the same track where it
  was produced, then the chain of Geant4 tracks back to the primary, then the
  primary's mothers in the generator record with their own status.
  `mothers` and `mother_id` index into `plane_particles`. The primary's own
  entry is the generator's particle with status 201.
- The same tracks, the marked ones and every Geant4 ancestor, are also
  written to `sim_particles`, even those below `particle_ke_cut`, so the
  full history of each marked particle can be followed there by `parent_id`.
  Other tracks below the cut are still left out.

Since only the marked tracks have status 1, the second stage tracks exactly
those (see [Primary selection](#primary-selection)). Two rules keep a track
from being replayed twice. The recording plane must `kill`, so nothing a
marked track produces after crossing can reach it, and only one plane may
record. Secondaries it produced before the plane, such as delta rays, can
still cross and be marked. They are replayed too: the second stage starts
their ancestor at the plane, so it does not produce them again.

`file_source` reads the field back with `product: 'plane_particles'`. Its
`product` key names any `std::vector<SHiP::MCParticle>` field of the input
(`mc_particles` by default). It passes on the input's event header and, when
that has no `original_event_id` yet, sets it to the first run's
`event_number`, so each
replayed event can be matched to the event it came from even when the first
run dropped empty events.

```bash
pixi run phlex -c <(jsonnet --tla-code seed=42 --tla-code events=10000 \
  workflows/fixed_target_production.jsonnet)
n=$(pixi run python3 scripts/count_entries.py fixed_target_production.root)
pixi run phlex -c <(jsonnet --tla-str infile=fixed_target_production.root \
  --tla-code events="$n" --tla-code seed=43 workflows/plane_replay.jsonnet)
```

The second stage's geometry must not contain the killing plane: the replayed
particles start on its surface and would be stopped at once.
`plane_replay.jsonnet` therefore has no scoring planes. The `scoring_planes`
test runs both stages on the builtin geometry.
