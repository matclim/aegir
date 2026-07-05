// lib.libsonnet — building blocks shared by the aegir workflows.
//
// Workflows compose these with jsonnet object merging, e.g.
//   lib.geant4 { concurrency: 4 }
// overrides or extends a block without repeating it.
{
  driver(events):: {
    cpp: 'generate_layers',
    layers: {
      event: { total: events },
    },
  },

  // ── sources ────────────────────────────────────────────────────────────
  null_field:: { cpp: 'field_null_provider' },

  covfie_field(magnets):: {
    cpp: 'field_covfie_provider',
    magnets: magnets,
  },

  // 0.5 T constant By over the whole World volume — the field-smoke setup.
  world_field_05T_y:: self.covfie_field([
    {
      name: 'WorldField',
      volume_pattern: 'World',
      cvf_file: 'world_05T_y.cvf',
    },
  ]),

  builtin_geometry:: { cpp: 'geometry_builtin_provider' },

  geomodel_geometry:: {
    cpp: 'geometry_geomodel_provider',
    db_file: 'ship_geometry.db',
    sensitive_volumes: ['ScoringPlane'],
  },

  // Default spray gun: 10–100 GeV μ− in a narrow cone from z = −500 mm.
  gun:: {
    cpp: 'particle_gun_source',
    pdg: 13,
    p_min: 10.0,
    p_max: 100.0,
    max_theta: 0.1,
    vertex_z: -500.0,
  },

  // Pencil beam for the field smokes: straight 20 GeV μ− from z = −2 m.
  pencil_gun:: {
    cpp: 'particle_gun_source',
    pdg: 13,
    p_min: 20.0,
    p_max: 20.0,
    max_theta: 0.0,
    vertex_z: -2000.0,
  },

  pythia8:: {
    cpp: 'pythia8_source',
    beam_energy: 400.0,
    process: 'SoftQCD:inelastic',
  },

  fixed_target:: {
    cpp: 'fixed_target_source',
    beam_energy: 400.0,
    target_z: 74,
    target_a: 184,
    // Target z extents to be determined from GeoModel geometry
    target_z_start: 0.0,
    target_z_end: 1164.0,
    interaction_length: 191.9,
    tau0_threshold: 1.0,
  },

  // ── modules ────────────────────────────────────────────────────────────
  geant4:: {
    cpp: 'geant4_module',
    physics_list: 'FTFP_BERT',
    verbosity: 0,
  },

  // Crossing-mode Geant4 for the GeoModel workflows: scoring-plane SD with a
  // KE threshold, production-cut regions in the target/absorber, tracked-
  // particle KE cut.
  geant4_crossing:: {
    cpp: 'geant4_module',
    physics_list: 'FTFP_BERT',
    sd_mode: 'crossing',
    ke_threshold: 0.5,
    energy_cut: true,
    particle_ke_cut: 1.0,
    regions: { Target: 50, HadronAbsorber: 50 },
  },

  noop_output:: { cpp: 'sim_output_module', mode: 'noop' },

  mc_only_output(rntuple_file, histo_file):: {
    cpp: 'sim_output_module',
    mode: 'mc_only',
    rntuple_file: rntuple_file,
    histo_file: histo_file,
  },

  full_output(rntuple_file, histo_file):: {
    cpp: 'sim_output_module',
    mode: 'full',
    rntuple_file: rntuple_file,
    histo_file: histo_file,
  },
}
