local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    pythia8: lib.pythia8 {
      parallel: true,
      num_threads: 4,
      num_events: 100,
    },
  },
  modules: {
    geant4: lib.geant4 { concurrency: 4 },
    output: lib.full_output('pythia8_mt_output.root', 'pythia8_mt_validation.root'),
  },
}
