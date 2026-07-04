local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('num_events'))),
  sources: {
    field: lib.null_field,
    geometry: lib.geomodel_geometry,
    pythia8: lib.fixed_target,
  },
  modules: {
    geant4: lib.geant4_crossing {
      energy_cut_threshold: 30.0,
      concurrency: std.parseInt(std.extVar('concurrency')),
    },
    output: lib.noop_output,
  },
}
