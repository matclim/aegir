// Single-magnet smoke for the field-service plumbing: builtin geometry
// (tungsten target + five scoring planes), constant 0.5 T By over the whole
// World volume, fixed 20 GeV muon offset in x so it misses the target.
// Without a field hits are at x = 200 mm everywhere; with the field the hits
// trace out a parabolic deflection in +x (μ−, By > 0).
local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100),
  sources: {
    geometry: lib.builtin_geometry,
    field: lib.covfie_field([
      {
        name: 'WorldField',
        volume_pattern: 'World',
        cvf_file: 'world_05T_y.cvf',
      },
    ]),
    gun: lib.pencil_gun { vertex_x: 200.0 },
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output('gun_st_field_smoke.root', 'gun_st_field_smoke_validation.root'),
  },
}
