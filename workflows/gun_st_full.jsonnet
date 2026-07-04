local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('events'))),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    gun: lib.gun,
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output('bench_gun_st_output.root', 'bench_gun_st_validation.root'),
  },
}
