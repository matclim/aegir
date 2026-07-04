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
    output: lib.noop_output,
  },
}
