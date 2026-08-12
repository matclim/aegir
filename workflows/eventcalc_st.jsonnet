// Read LLP decays from an EventCalc-SHiP .dat record and run the decay
// products through Geant4. See docs/eventcalc.md.
//   n=$(pixi run count_eventcalc_events HNL_..._data.dat)
//   jsonnet --ext-str events=$n --ext-str infile=HNL_..._data.dat \
//       --ext-str simout=sim.root --ext-str histo=valid.root \
//       workflows/eventcalc_st.jsonnet
local lib = import 'lib.libsonnet';
local n_events = std.parseInt(std.extVar('events'));
{
  driver: lib.driver(n_events),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    eventcalc: lib.eventcalc { file: std.extVar('infile') },
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output(std.extVar('simout'), std.extVar('histo')),
  },
}
