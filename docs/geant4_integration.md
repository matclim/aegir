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
      regions: { Target: 50, HadronAbsorber: 50 },
    },
  },
}
```
