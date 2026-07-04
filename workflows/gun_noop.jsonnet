local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('events'))),
  sources: {
    gun: lib.gun,
  },
  modules: {
    output: lib.noop_output,
  },
}
