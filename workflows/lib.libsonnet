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

  // Magnets, maps and their placements from a placement manifest (field_service
  // docs/placements_format.md). The default is the SHiP muon shield and
  // spectrometer manifest that ship-field-maps installs under
  // $SHIPFIELD_ROOT/share/field/.
  field_placements(manifest='placements.json'):: {
    cpp: 'field_map_provider',
    placements: manifest,
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

  // Both Pythia8 sources take any Pythia8 setting: `pythia_settings`, a list
  // of 'Key = value' lines, and/or `pythia_cmnd`, a command file; applied
  // after aegir's own settings, before init (docs/physics_parameters.md).
  //
  // `record` selects how much of the generator's event record is emitted:
  // 'final_state' (only what Geant4 will track) or 'full' (the whole record,
  // with HepMC statuses and mother links intact — several times larger; the
  // geant4 module still tracks only status-1 entries). See
  // docs/generator_record.md.
  pythia8:: {
    cpp: 'pythia8_source',
    stage: $.stage,
    beam_energy: 400.0,  // GeV
    process: 'SoftQCD:inelastic',
    record: 'final_state',
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
    record: 'final_state',
  },

  // Neutrino interactions pre-generated with GENIE, read from a rootracker
  // file (gntpc -f rootracker); see docs/genie.md for how to produce one.
  genie_reader:: {
    cpp: 'genie_reader_source',
    stage: $.stage,
    file: 'genie_events.rootracker.root',
    record: 'final_state',
  },

  // ── modules ────────────────────────────────────────────────────────────
  // Without a `seed`, each run draws a random one (logged at startup, so a
  // run can be reproduced after the fact). Merge one in for reproducible
  // output, e.g. lib.geant4 { seed: 12345 }.
  // Physics parameters: hadronic_parameters, hadronic_flags,
  // developer_parameters, developer_flags and commands (Geant4 UI commands),
  // applied in PreInit (docs/physics_parameters.md).
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
  // hadron absorber, in the air gap before the first muon-shield
  // magnet (absorber end at z = 4350 mm, magn_1 from 4550 mm), with
  // FairShip's size (3.56 m - 1 mm by 1.7 m - 1 mm half-widths, 1 mm
  // half-thickness). That is wider than the muon-shield container
  // (±1810 mm in x), which a scoring plane may be: it lives in its own
  // parallel world, so it also records what passes beside the shield.
  // It records every track reaching it — muons, neutrinos and all others —
  // and stops it, so nothing is tracked beyond. The tracks it records going
  // downstream are also written as plane_particles, as they entered the
  // plane and with their ancestry, for workflows/plane_replay.jsonnet to
  // simulate further. Merge in a `ke_threshold` (GeV) to match the run's energy cut.
  exit_hadron_absorber_plane:: {
    name: 'exit_hadron_absorber',
    id: 1,
    half_x: 3559.0,  // mm
    half_y: 1699.0,  // mm
    half_z: 1.0,  // mm
    after_volume: '/SHiP/muon_shield/magn_absorb',
    gap: 10.0,  // mm
    kill: true,
    record_particles: true,
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

  // ── physics parameters ─────────────────────────────────────────────────
  // A parameter point, e.g. for a tune: the keys it may set on a Pythia8
  // source and on the geant4 module (docs/physics_parameters.md).
  //   {
  //     pythia: { pythia_settings: ['StringFlav:probStoUD = 0.22'] },
  //     geant4: { hadronic_flags: { apply_factor_xs: true },
  //               hadronic_parameters: { xs_factor_pion_inelastic: 1.05 },
  //               commands: ['/physics_lists/em/GammaToMuons true'] },
  //   }
  tune_keys:: {
    pythia: ['pythia_settings', 'pythia_cmnd'],
    geant4: ['physics_list', 'hadronic_parameters', 'hadronic_flags',
             'developer_parameters', 'developer_flags', 'commands'],
  },

  // The part `part` ('pythia' or 'geant4') of a parameter point, checked
  // against tune_keys so that a misspelt key fails instead of being ignored.
  // Merge it into the block: lib.fixed_target { ... } + lib.tune_part(t, 'pythia').
  tune_part(tune, part)::
    local unknown_parts = std.setDiff(std.set(std.objectFields(tune)),
                                      std.set(std.objectFields($.tune_keys)));
    local block = std.get(tune, part, {});
    local unknown = std.setDiff(std.set(std.objectFields(block)),
                                std.set($.tune_keys[part]));
    assert std.length(unknown_parts) == 0 :
           'parameter point: unknown part(s) ' + std.join(', ', unknown_parts) +
           ' (expected pythia, geant4)';
    assert std.length(unknown) == 0 :
           'parameter point: unknown ' + part + ' key(s) ' + std.join(', ', unknown) +
           ' (allowed: ' + std.join(', ', $.tune_keys[part]) + ')';
    block,

  // The workflow with its whole configuration stored in the output file as
  // the TNamed `run_info` (JSON), so the output records the parameter point,
  // seeds and settings that made it. `output` names the sim_output_module.
  with_run_info(workflow, output='output')::
    workflow + {
      modules+: { [output]+: { run_info: std.manifestJsonMinified(workflow) } },
    },
}
