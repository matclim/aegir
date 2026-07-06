local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    genie_reader: lib.genie_reader,
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output('genie_reader_st_output.root', 'genie_reader_st_validation.root'),
  },
}
