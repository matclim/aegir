// Single-magnet smoke for the field-service plumbing: builtin geometry
// (tungsten target + five scoring planes), constant 0.5 T By over the whole
// World volume, fixed 20 GeV muon offset in x so it misses the target.
// Without a field hits are at x = 200 mm everywhere; with the field the hits
// trace out a parabolic deflection in +x (μ−, By > 0).
{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 100 },
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
    },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      rntuple_file: 'gun_st_field_smoke.root',
      histo_file: 'gun_st_field_smoke_validation.root',
    },
  },
}
