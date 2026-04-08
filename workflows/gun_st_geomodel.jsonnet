{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 10 },
    },
  },
  sources: {
    geometry: {
      cpp: 'geometry_geomodel_provider',
      db_file: '../geometry-target-only/build/apps/ship_geometry.db',
      sensitive_volumes: ['ScoringPlane'],
    },
    gun: {
      cpp: 'particle_gun_source',
      pdg: 13,
      p_min: 10.0,
      p_max: 100.0,
      max_theta: 0.1,
      vertex_z: -500.0,
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
      rntuple_file: 'geomodel_output.root',
      histo_file: 'geomodel_validation.root',
    },
  },
}
