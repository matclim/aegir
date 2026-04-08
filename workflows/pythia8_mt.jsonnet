{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 100 },
    },
  },
  sources: {
    geometry: { cpp: 'geometry_builtin_provider' },
    pythia8: {
      cpp: 'pythia8_source',
      beam_energy: 400.0,
      process: 'SoftQCD:inelastic',
      parallel: true,
      num_threads: 4,
      num_events: 100,
    },
  },
  modules: {
    geant4: {
      cpp: 'geant4_module',
      physics_list: 'FTFP_BERT',
      verbosity: 0,
      concurrency: 4,
    },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      rntuple_file: 'pythia8_mt_output.root',
      histo_file: 'pythia8_mt_validation.root',
    },
  },
}
