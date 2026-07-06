# Neutrino events from GENIE

aegir reads neutrino interactions pre-generated with [GENIE](http://www.genie-mc.org)
through the `genie_reader_source` plugin. Event generation runs outside aegir
with GENIE's own tools; aegir consumes the resulting **rootracker** file — a
plain ROOT tree that keeps the full particle record (status codes and mother
links), so no GENIE libraries are needed to read it. This also keeps
GENIE-licensed code out of aegir: GENIE is distributed under GPL-style terms,
while everything here only touches ROOT files.

A tighter integration — calling GENIE as a library so that the neutrino flux
and the detector geometry enter the interaction sampling directly — is planned
as a separate, optional plugin package. The reader below stays useful as the
validation baseline for that.

## Producing an input file

You need a GENIE installation (not part of the aegir environment yet). Three
steps, mirroring the FairShip procedure but ending in `rootracker` rather than
the flattened `gst` format:

1. **Cross-section splines** — either download pre-computed splines for your
   tune from [scisoft.fnal.gov](https://scisoft.fnal.gov/scisoft/packages/genie_xsec/)
   or compute them for the target nuclei of interest, e.g. tungsten:

   ```sh
   gmkspl -p 12,-12,14,-14,16,-16 -t 1000741840 -e 350 -o xsec_splines.xml
   ```

2. **Generate events.** With `gevgen_fnal` (recommended) GENIE traces the
   neutrino flux through the detector geometry (a GDML file also works as
   input) and places interaction vertices material-by-material, so the
   rootracker file already carries vertices in detector coordinates. Plain
   `gevgen` (single nucleus, no geometry) also works, but then every event
   sits at the origin and the vertex distribution is not physical — fine for
   technical checks only.

3. **Convert** the native GENIE output to rootracker:

   ```sh
   gntpc -i events.ghep.root -f rootracker -o events.rootracker.root
   ```

## Running

```sh
pixi run phlex -c workflows/genie_reader_st.jsonnet
```

The source block accepts:

| key | default | meaning |
|:---|:---|:---|
| `file` | (required) | rootracker ROOT file |
| `tree` | `gRooTracker` | tree name inside the file |
| `first_entry` | `0` | skip this many events (e.g. to split a file across jobs) |

Events are read sequentially: workflow event *N* is file entry
`first_entry + N`. Requesting more events than the file holds is an error, not
silently-empty events — reduce the driver's event count or supply a larger
file.

## What is read

Only stable final-state particles (GENIE status 1) are handed to Geant4; the
incoming neutrino, the struck nucleus and intermediate states are dropped.
All particles of an event share the interaction vertex (`EvtVtx`), converted
from GENIE's SI units to aegir's mm/ns. `motherId` is remapped to index the
emitted collection and is `-1` when the mother was not itself final state —
the common case.
