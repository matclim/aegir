local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('events'))),
  sources: {
    pythia8: lib.pythia8,
  },
  modules: {
    output: lib.noop_output,
  },
}
