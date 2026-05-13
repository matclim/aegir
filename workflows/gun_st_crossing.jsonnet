{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 1000 },
    },
  },
  sources: {
    geometry: {
      cpp: 'geometry_geomodel_provider',
      db_file: 'ship_geometry.db',
      sensitive_volumes: ['ScoringPlane'],
    },
    gun: {
      cpp: 'particle_gun_source',
      pdg: 13,
      p_min: 50.0,
      p_max: 400.0,
      max_theta: 0.05,
      vertex_z: -500.0,
    },
  },
  modules: {
    geant4: {
      cpp: 'geant4_module',
      physics_list: 'FTFP_BERT',
      sd_mode: 'crossing',
      ke_threshold: 0.5,
      energy_cut: true,
      particle_ke_cut: 1.0,
      regions: { Target: 50, HadronAbsorber: 50 },
    },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      rntuple_file: 'gun_crossing_output.root',
      histo_file: 'gun_crossing_validation.root',
    },
  },
}
