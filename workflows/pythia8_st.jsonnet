local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    pythia8: lib.pythia8,
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output('pythia8_st_output.root', 'pythia8_st_validation.root'),
  },
}
