local lib = import 'lib.libsonnet';
local num_events = std.parseInt(std.extVar('num_events'));
{
  driver: lib.driver(num_events),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    pythia8: lib.pythia8 {
      parallel: true,
      num_threads: std.parseInt(std.extVar('pythia_threads')),
      num_events: num_events,
    },
  },
  modules: {
    geant4: lib.geant4 { concurrency: std.parseInt(std.extVar('concurrency')) },
  },
}
