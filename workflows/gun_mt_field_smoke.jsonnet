// Multi-threaded smoke with the covfie field: same setup as
// gun_st_field_smoke.jsonnet but with 4 worker threads. Validates that the
// per-call covfie field_view construction is thread-safe and that the
// G4FieldManager wiring survives the master/worker handoff.
local lib = import 'lib.libsonnet';
{
  driver: lib.driver(200),
  sources: {
    geometry: lib.builtin_geometry,
    field: lib.world_field_05T_y,
    gun: lib.pencil_gun { vertex_x: 200.0 },
  },
  modules: {
    geant4: lib.geant4 { concurrency: 4 },
    output: lib.full_output('gun_mt_field_smoke.root', 'gun_mt_field_smoke_validation.root'),
  },
}
