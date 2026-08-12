<!--
SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration

SPDX-License-Identifier: LGPL-3.0-or-later
-->

# LLP decays from EventCalc

aegir reads long-lived-particle decays pre-generated with
[EventCalc-SHiP](https://github.com/maksymovchynnikov/EventCalc-SHiP) through
the `eventcalc_source` plugin. Phenomenology runs outside aegir with
EventCalc's own tools; aegir consumes the resulting `*_data.dat` record — a
plain-text table of sampled decays inside the decay volume — so no EventCalc
installation is needed to read it, and the LLP model space stays decoupled
from the aegir build.

EventCalc samples an LLP's production, propagation and decay analytically: it
draws the LLP kinematics from the production spectrum, requires the trajectory
to intersect the decay volume, places a decay vertex there, and decays the LLP
into a visible final state. What reaches aegir is therefore already a decay
that happened — the plugin hands Geant4 the *decay products*, not the LLP.

## Producing an input file

You need an EventCalc checkout (not part of the aegir environment). The
interactive driver walks through the choices:

```sh
python simulate.py
```

You are asked for the LLP (HNL, dark scalar, ALP, …), its mixing pattern where
applicable, a list of masses and lifetimes, the decay channels to include, and
the number of events. Output lands in
`outputs/<LLP>/eventData/<LLP>_<mass>_<lifetime>_data.dat`, one file per
mass–lifetime point.

The non-interactive path takes the same choices as flags, which is what you
want for a scan:

```sh
python simulate.py --llp HNL --mixing-pattern "1 0 0" \
    --mass 1.0 --c-tau 0.1 --nevents 100000
```

SHiP maintains a mirror at [ShipSoft/EventCalc](https://github.com/ShipSoft/EventCalc);
prefer it if the two ever diverge, since that is the version the collaboration
supports.

## Running

The driver's event count must match the number of decays in the file, so the
count is read out first — the same pattern `file_source` uses (see
[count_entries.py](../scripts/count_entries.py)):

```sh
n=$(pixi run count_eventcalc_events HNL_1.000e+00_1.000e-01_data.dat)
pixi run phlex -c <(jsonnet --ext-str events="$n" \
    --ext-str infile=HNL_1.000e+00_1.000e-01_data.dat \
    --ext-str simout=eventcalc_sim.root --ext-str histo=eventcalc_val.root \
    workflows/eventcalc_st.jsonnet)
```

The source block accepts:

| key | default | meaning |
|:---|:---|:---|
| `file` | (required) | EventCalc `*_data.dat` record |
| `first_event` | `0` | skip this many decays (e.g. to split a file across jobs) |
| `skip_neutrinos` | `true` | drop neutrino daughters instead of tracking them |
| `emit_llp` | `false` | also emit the LLP itself (see below) |
| `offset_x`, `offset_y`, `offset_z` | `0.0` | shift applied to the decay vertex [mm] |

Events are read in order: workflow event *N* is file row `first_event + N`.
Requesting more events than the file holds is an error, not silently-empty
events — reduce the driver's event count or supply a larger file.

Unlike `genie_reader_source`, the reader is immutable once constructed, so
events are served with unlimited concurrency and the multi-threaded workflows
are not serialised on input.

## Format

The first line carries the normalisation of the whole sample: the squared
coupling, the total number of LLPs produced over 15 years of SHiP running, the
polar and azimuthal acceptances, the mean decay probability, the visible
branching ratio and the resulting expected number of events. Blocks of decays
then follow, each introduced by a `#<process=...; sample_points=N>` header
naming the decay channel.

Each row is one decay: ten columns for the LLP —
`px py pz E m PDG P_decay x_decay y_decay z_decay` — followed by groups of six
per decay product, `px py pz E m PDG`. Channels of different multiplicity are
stacked into one file, so short rows are padded with groups of
`0. 0. 0. 0. 0. -999.`.

> The `-999` marks a **whole padding group**, not a single missing value.
> Filtering it out value-by-value leaves five zeros behind and shifts every
> subsequent group by one column, silently producing extra daughters with
> `PDG = 0`. `eventcalc_reader.hpp` tests the group's PDG column and stops.

Momenta, energies and masses are in GeV; the decay vertex is in **metres**,
with the origin at the centre of the SHiP target. Both acquire their unit at
the read site in `eventcalc_source.cpp` and convert to the canonical storage
units, per [units.md](units.md). If the configured geometry provider puts its
origin elsewhere, correct for it with the `offset_*` keys.

## What is read

The decay products are emitted as Geant4 primaries, all sharing the decay
vertex. EventCalc records no time, so it is reconstructed from the LLP
kinematics: the particle leaves the target at *t* = 0 and reaches the vertex
after a path *s* at speed *βc*, giving *t* = *s*/*βc*. Neglecting the flight of
the parent meson is a sub-nanosecond approximation, but setting *t* = 0 would
not be — a vertex tens of metres downstream would arrive impossibly early for
the timing detector.

`skip_neutrinos` drops daughter neutrinos by default: EventCalc records them
for completeness, Geant4 will happily track them, and they deposit nothing.

The LLP itself is **not** emitted by default. Geant4 has no particle
definition for an HNL or a dark scalar, and the decay has already happened, so
there is nothing to track. `emit_llp` adds it as `status = 2` with the
daughters' `motherId` pointing at it, which is only useful if you have
registered a matching `G4ParticleDefinition` or want the LLP kinematics
carried through to the output for truth studies.

## Weights

Every row carries `P_decay,LLP`, the probability that this LLP decays inside
the volume. It is a **per-event weight**: the rows are not equally probable,
and an unweighted distribution built from them has no physical meaning.

The weight is published as `SHiP::EventHeader::weight` on the `event` layer
and written to the `event_header` field of the output RNTuple, so any analysis
downstream must weight by it. `EventHeader::original_event_id` records the row
of the input file the event came from, which survives splitting a file across
jobs with `first_event`.

Sources that sample uniformly (the gun, Pythia8, GENIE) publish the default
header — weight 1.0, `original_event_id` −1 — so the output schema is the same
whether or not the input was weighted.

The absolute normalisation follows from the first line of the input,

```
N_events = N_LLPs_produced x eps_polar x eps_azimuthal x Br_visible x mean(P_decay)
```

which EventCalc also reports directly as the expected number of events. Note
that the mean is over *events*, not over particles: summing weights across the
`mc_particles` collection instead of across entries overcounts by the daughter
multiplicity.
