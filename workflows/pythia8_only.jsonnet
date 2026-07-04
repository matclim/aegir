local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('events'))),
  sources: {
    pythia8: lib.pythia8,
  },
  modules: {
    output: lib.mc_only_output('pythia8_only_output.root', 'pythia8_only_validation.root'),
  },
}
