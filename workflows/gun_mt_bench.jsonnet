local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('num_events'))),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    gun: lib.gun,
  },
  modules: {
    geant4: lib.geant4 { concurrency: std.parseInt(std.extVar('concurrency')) },
  },
}
