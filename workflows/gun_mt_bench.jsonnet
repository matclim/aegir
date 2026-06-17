local concurrency = std.parseInt(std.extVar('concurrency'));

{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: std.parseInt(std.extVar('num_events')) },
    },
  },
  sources: {
    field: { cpp: 'field_null_provider' },
    geometry: { cpp: 'geometry_builtin_provider' },
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
      concurrency: concurrency,
    },
  },
}
