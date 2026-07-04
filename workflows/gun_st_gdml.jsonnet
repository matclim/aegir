local lib = import 'lib.libsonnet';
{
  driver: lib.driver(10),
  sources: {
    field: lib.null_field,
    geometry: {
      cpp: 'geometry_gdml_provider',
      gdml_file: '../geometry-target-only/ship_target_only.gdml',
      sensitive_volumes: ['ScoringPlane'],
    },
    gun: lib.gun,
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output('gdml_output.root', 'gdml_validation.root'),
  },
}
