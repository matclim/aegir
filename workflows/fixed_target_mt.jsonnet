local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100000),
  sources: {
    field: lib.null_field,
    geometry: lib.geomodel_geometry,
    pythia8: lib.fixed_target,
  },
  modules: {
    geant4: lib.geant4_crossing {
      energy_cut_threshold: 30.0,
      concurrency: 4,
    },
    output: lib.full_output('fixed_target_mt_output.root', 'fixed_target_mt_validation.root') {
      filter_empty: true,
    },
  },
}
