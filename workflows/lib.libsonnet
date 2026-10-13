// lib.libsonnet — building blocks shared by the aegir workflows.
//
// Workflows compose these with jsonnet object merging, e.g.
//   lib.geant4 { concurrency: 4 }
// overrides or extends a block without repeating it.
{
  // Phlex requires every job to name its stage; products are labelled with
  // it. Workflows set `stage: lib.stage` at the top level. Phlex does not
  // pass that key on to sources, so the generator sources below carry their
  // own copy for the products they publish.
  stage:: 'simulation',

  driver(events):: {
    cpp: 'generate_layers',
    layers: {
      event: { total: events },
    },
  },

  // ── sources ────────────────────────────────────────────────────────────
  null_field:: { cpp: 'field_null_provider' },

  // Field maps in the SHiP field-map format. Each magnet names a `file`
  // (bare names resolve via $SHIPFIELD_ROOT/share/field/), the `map` inside
  // it, and optionally a `translation` [x, y, z] in mm placing the map's
  // origin in the global frame.
  field_map(magnets):: {
    cpp: 'field_map_provider',
    magnets: magnets,
  },

  // 0.5 T constant By over the whole World volume — the field-smoke setup.
  world_field_05T_y:: self.field_map([
    {
      name: 'WorldField',
      volume_pattern: 'World',
      file: 'world_05T_y.root',
      map: 'world_05T_y',
    },
  ]),

  builtin_geometry:: { cpp: 'geometry_builtin_provider' },

  geomodel_geometry:: {
    cpp: 'geometry_geomodel_provider',
    db_file: 'ship_geometry.db',
    sensitive_volumes: ['ScoringPlane'],
  },

  // The generator sources follow the same seeding convention as the geant4
  // module below: without a `seed`, each run draws a random one (logged at
  // startup, so a run can be reproduced after the fact). Merge one in for
  // reproducible primaries, e.g. lib.gun { seed: 12345 }.
  // Default spray gun: 10–100 GeV μ− in a narrow cone from z = −500 mm.
  gun:: {
    cpp: 'particle_gun_source',
    stage: $.stage,
    pdg: 13,
    p_min: 10.0,  // GeV/c
    p_max: 100.0,  // GeV/c
    max_theta: 0.1,  // rad
    vertex_z: -500.0,  // mm
  },

  // Pencil beam for the field smokes: straight 20 GeV μ− from z = −2 m.
  pencil_gun:: {
    cpp: 'particle_gun_source',
    stage: $.stage,
    pdg: 13,
    p_min: 20.0,  // GeV/c
    p_max: 20.0,  // GeV/c
    max_theta: 0.0,  // rad
    vertex_z: -2000.0,  // mm
  },

  pythia8:: {
    cpp: 'pythia8_source',
    stage: $.stage,
    beam_energy: 400.0,  // GeV
    process: 'SoftQCD:inelastic',
  },

  fixed_target:: {
    cpp: 'fixed_target_source',
    stage: $.stage,
    beam_energy: 400.0,  // GeV
    target_z: 74,  // proton number, not a coordinate
    target_a: 184,
    // Target z extents to be determined from GeoModel geometry
    target_z_start: 0.0,  // mm
    target_z_end: 1164.0,  // mm
    interaction_length: 191.9,  // mm
    tau0_threshold: 1.0,  // mm/c
  },

  // Neutrino interactions pre-generated with GENIE, read from a rootracker
  // file (gntpc -f rootracker); see docs/genie.md for how to produce one.
  genie_reader:: {
    cpp: 'genie_reader_source',
    stage: $.stage,
    file: 'genie_events.rootracker.root',
  },

  // ── modules ────────────────────────────────────────────────────────────
  // Without a `seed`, each run draws a random one (logged at startup, so a
  // run can be reproduced after the fact). Merge one in for reproducible
  // output, e.g. lib.geant4 { seed: 12345 }.
  geant4:: {
    cpp: 'geant4_module',
    physics_list: 'FTFP_BERT',
    verbosity: 0,
  },

  // Crossing-mode Geant4 for the GeoModel workflows: scoring-plane SD with a
  // KE threshold, production-cut regions in the target/absorber, tracked-
  // particle KE cut. Seeding works as for `geant4` above.
  geant4_crossing:: {
    cpp: 'geant4_module',
    physics_list: 'FTFP_BERT',
    sd_mode: 'crossing',
    ke_threshold: 0.5,  // GeV
    energy_cut: true,
    particle_ke_cut: 1.0,  // GeV
    regions: { '/SHiP/target': 50, '/SHiP/muon_shield/magn_absorb': 50 },  // production cuts [mm]
  },

  // FairShip's exitHadronAbsorber scoring plane for the GeoModel geometry, as
  // an entry for the geant4 module's `scoring_planes`: 1 cm behind the
  // magnetised hadron absorber, in the air gap before the first muon-shield
  // magnet (absorber end at z = 4350 mm, magn_1 from 4550 mm), with
  // FairShip's size (3.56 m - 1 mm by 1.7 m - 1 mm half-widths, 1 mm
  // half-thickness). That is wider than the muon-shield container
  // (±1810 mm in x), which a scoring plane may be: it lives in its own
  // parallel world, so it also records what passes beside the shield.
  // It records every track reaching it — muons, neutrinos and all others —
  // and stops it, so nothing is tracked beyond. Merge in a `ke_threshold`
  // (GeV) to match the run's energy cut.
  exit_hadron_absorber_plane:: {
    name: 'exit_hadron_absorber',
    id: 1,
    half_x: 3559.0,  // mm
    half_y: 1699.0,  // mm
    half_z: 1.0,  // mm
    after_volume: '/SHiP/muon_shield/magn_absorb',
    gap: 10.0,  // mm
    kill: true,
  },

  noop_output:: { cpp: 'sim_output_module', mode: 'noop' },

  // The writing modes also accept two optional keys bounding the writer's
  // memory use (issue #77): cluster_size_mib (default 32) and fill_contexts
  // (default 4, or the phlex thread count if lower). Override by merging, e.g.
  //   lib.full_output(f, h) { cluster_size_mib: 16, fill_contexts: 8 }
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
