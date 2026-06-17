{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 100000 },
    },
  },
  sources: {
    field: { cpp: 'field_null_provider' },
    geometry: {
      cpp: 'geometry_geomodel_provider',
      db_file: 'ship_geometry.db',
      sensitive_volumes: ['ScoringPlane'],
    },
    pythia8: {
      cpp: 'fixed_target_source',
      beam_energy: 400.0,
      target_z: 74,
      target_a: 184,
      target_z_start: 0.0,
      target_z_end: 1164.0,
      interaction_length: 191.9,
      tau0_threshold: 1.0,
    },
  },
  modules: {
    geant4: {
      cpp: 'geant4_module',
      physics_list: 'FTFP_BERT',
      sd_mode: 'crossing',
      ke_threshold: 0.5,
      energy_cut_threshold: 30.0,
      energy_cut: true,
      particle_ke_cut: 1.0,
      regions: { Target: 50, HadronAbsorber: 50 },
      concurrency: 4,
    },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      filter_empty: true,
      rntuple_file: 'fixed_target_mt_output.root',
      histo_file: 'fixed_target_mt_validation.root',
    },
  },
}
