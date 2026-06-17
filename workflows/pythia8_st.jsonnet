{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 100 },
    },
  },
  sources: {
    field: { cpp: 'field_null_provider' },
    geometry: { cpp: 'geometry_builtin_provider' },
    pythia8: {
      cpp: 'pythia8_source',
      beam_energy: 400.0,
      process: 'SoftQCD:inelastic',
    },
  },
  modules: {
    geant4: {
      cpp: 'geant4_module',
      physics_list: 'FTFP_BERT',
      verbosity: 0,
    },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      rntuple_file: 'pythia8_st_output.root',
      histo_file: 'pythia8_st_validation.root',
    },
  },
}
