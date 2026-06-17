// No-field control matching gun_st_field_smoke.jsonnet — same builtin geometry
// and same 20 GeV μ− offset in x; field_null_provider means no deflection.
{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: 100 },
    },
  },
  sources: {
    geometry: { cpp: 'geometry_builtin_provider' },
    field: { cpp: 'field_null_provider' },
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
      rntuple_file: 'gun_st_nofield_smoke.root',
      histo_file: 'gun_st_nofield_smoke_validation.root',
    },
  },
}
