// Fixed-target production with FairShip's scoring plane behind the hadron
// absorber (FairShip's run_fixedTarget.py): Pythia8 p-W interactions in the
// GeoModel target, tracked through the target and hadron absorber. Every
// particle reaching the plane with at least `ecut` kinetic energy — muons,
// neutrinos and all others — is recorded there as a SimHit with detectorId
// 100 and geometryNodeId 1, and stopped. Tracks below `ecut` are killed
// wherever they are, as in FairShip; events without a plane hit are not
// written.
//
// Top-level arguments (--tla-code name=value):
//   seed    seeds the generator and Geant4, for reproducible batch jobs;
//           without it each run draws its own (logged at startup)
//   ecut    energy cut in GeV (FairShip's --ecut, default 0.5)
//   events  number of proton interactions
//
// Needs the GeoModel geometry DB (pixi run build_geometry, see README).
local lib = import 'lib.libsonnet';
function(seed=null, ecut=0.5, events=100000) {
  stage: lib.stage,
  driver: lib.driver(events),
  sources: {
    field: lib.null_field,
    // Only the scoring plane is instrumented.
    geometry: lib.geomodel_geometry { sensitive_volumes: [] },
    pythia8: lib.fixed_target { [if seed != null then 'seed']: seed },
  },
  modules: {
    geant4: lib.geant4 {
      energy_cut: true,
      energy_cut_threshold: ecut,
      // Tracks below ecut are killed on their first step: don't record them.
      particle_ke_cut: ecut,
      regions: lib.geant4_crossing.regions,
      scoring_planes: [lib.exit_hadron_absorber_plane { ke_threshold: ecut }],
      [if seed != null then 'seed']: seed,
    },
    output: lib.full_output('fixed_target_production.root', 'fixed_target_production_validation.root') {
      filter_empty: true,
    },
  },
}
