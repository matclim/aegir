// Per-region locality smoke: field is installed ONLY on the Target volume
// (z ∈ [-500, 500] mm). A straight 20 GeV μ− through the centre of the target
// must bend inside it and then drift on a straight line — Δx(z) parabolic
// inside, linear outside. The field is small (By=2T over 1 m), so the muon
// stays inside the world. Verifies the per-magnet G4FieldManager wiring:
// Geant4 must NOT call GetFieldValue while stepping outside the target.
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
          name: 'TargetField',
          volume_pattern: 'Target',
          cvf_file: 'target_2T_y.cvf',
        },
      ],
    },
    gun: {
      cpp: 'particle_gun_source',
      pdg: 13,
      p_min: 20.0,
      p_max: 20.0,
      max_theta: 0.0,
      vertex_z: -2000.0,  // on axis, into target
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
      rntuple_file: 'gun_st_field_local.root',
      histo_file: 'gun_st_field_local_validation.root',
    },
  },
}
