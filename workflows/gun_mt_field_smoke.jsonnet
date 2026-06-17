// Multi-threaded smoke with the covfie field: same setup as
// gun_st_field_smoke.jsonnet but with 4 worker threads. Validates that the
// per-call covfie field_view construction is thread-safe and that the
// G4FieldManager wiring survives the master/worker handoff.
{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 200 },
    },
  },
  sources: {
    geometry: { cpp: 'geometry_builtin_provider' },
    field: {
      cpp: 'field_covfie_provider',
      magnets: [
        {
          name: 'WorldField',
          volume_pattern: 'World',
          cvf_file: 'world_05T_y.cvf',
        },
      ],
    },
    gun: {
      cpp: 'particle_gun_source',
      pdg: 13,
      p_min: 20.0,
      p_max: 20.0,
      max_theta: 0.0,
      vertex_x: 200.0,
      vertex_z: -2000.0,
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
      rntuple_file: 'gun_mt_field_smoke.root',
      histo_file: 'gun_mt_field_smoke_validation.root',
    },
  },
}
