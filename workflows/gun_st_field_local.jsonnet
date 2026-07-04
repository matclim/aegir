// Per-region locality smoke: field is installed ONLY on the Target volume
// (z ∈ [-500, 500] mm). A straight 20 GeV μ− through the centre of the target
// must bend inside it and then drift on a straight line — Δx(z) parabolic
// inside, linear outside. The field is small (By=2T over 1 m), so the muon
// stays inside the world. Verifies the per-magnet G4FieldManager wiring:
// Geant4 must NOT call GetFieldValue while stepping outside the target.
local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100),
  sources: {
    geometry: lib.builtin_geometry,
    field: lib.covfie_field([
      {
        name: 'TargetField',
        volume_pattern: 'Target',
        cvf_file: 'target_2T_y.cvf',
      },
    ]),
    gun: lib.pencil_gun,  // on axis, into target
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output('gun_st_field_local.root', 'gun_st_field_local_validation.root'),
  },
}
