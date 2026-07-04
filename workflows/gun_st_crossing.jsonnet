local lib = import 'lib.libsonnet';
{
  driver: lib.driver(1000),
  sources: {
    field: lib.null_field,
    geometry: lib.geomodel_geometry,
    gun: lib.gun {
      p_min: 50.0,
      p_max: 400.0,
      max_theta: 0.05,
    },
  },
  modules: {
    geant4: lib.geant4_crossing,
    output: lib.full_output('gun_crossing_output.root', 'gun_crossing_validation.root'),
  },
}
