local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    gun: lib.gun,
  },
  modules: {
    geant4: lib.geant4 { concurrency: 4 },
    output: lib.full_output('gun_mt_output.root', 'gun_mt_validation.root'),
  },
}
