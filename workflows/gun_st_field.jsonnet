local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100),
  sources: {
    geometry: lib.geomodel_geometry,
    field: lib.covfie_field([
      {
        name: 'MuonShield',
        volume_pattern: 'MuonShield',
        cvf_file: 'muon_shield.cvf',
      },
      {
        name: 'Spectrometer',
        volume_pattern: 'SpectrometerDipole',
        cvf_file: 'spectrometer_dipole.cvf',
      },
    ]),
    gun: lib.gun,
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output('gun_st_field_output.root', 'gun_st_field_validation.root'),
  },
}
