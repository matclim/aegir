// Scoring-plane smoke on the builtin geometry (1 m tungsten target centred at
// the origin, silicon planes at z = 2, 4, ..., 10 m). A gun at the origin,
// inside the target, fires into a 0.5 rad cone; four scoring planes are laid
// over it in their parallel world:
//   1  box 1 cm behind the target (placed with after_volume),
//   2  cylindrical shell r = 80-81 mm around the target (placed by position),
//   3  box at z = 3 m that kills what it records,
//   4  small box cutting through the tungsten at z = 250 mm, which only a
//      parallel world allows.
// The sizes only need to produce these cases; they stand for no real setup.
// With the default neutrino gun nothing deposits energy, so every hit comes
// from the planes; scripts/scoring_planes.sh checks them geometrically.
// Pass --tla-code pdg=13 for muons (which also hit the silicon at z = 2 m,
// but no later silicon plane, since the plane at 3 m kills them).
local lib = import 'lib.libsonnet';
function(pdg=14, events=200) {
  stage: lib.stage,
  driver: lib.driver(events),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    gun: lib.gun {
      pdg: pdg,
      p_min: 10.0,
      p_max: 100.0,
      max_theta: 0.5,
      vertex_z: 0.0,
      seed: 20261012,
    },
  },
  modules: {
    geant4: lib.geant4 {
      seed: 20261012,
      scoring_planes: [
        {
          name: 'plane_after_target',
          id: 1,
          half_x: 1000.0,
          half_y: 1000.0,
          half_z: 1.0,
          after_volume: 'Target',
          gap: 10.0,
        },
        {
          name: 'plane_target_tube',
          id: 2,
          shape: 'tube',
          r_min: 80.0,
          r_max: 81.0,
          half_z: 500.0,
          position: [0.0, 0.0, 0.0],
        },
        {
          name: 'plane_kill',
          id: 3,
          half_x: 2000.0,
          half_y: 2000.0,
          half_z: 1.0,
          position: [0.0, 0.0, 3000.0],
          kill: true,
        },
        {
          name: 'plane_in_target',
          id: 4,
          half_x: 40.0,
          half_y: 40.0,
          half_z: 1.0,
          position: [0.0, 0.0, 250.0],
        },
      ],
    },
    output: lib.full_output('gun_st_scoring_planes.root', 'gun_st_scoring_planes_validation.root'),
  },
}
