// No-field control matching gun_st_field_smoke.jsonnet — same builtin geometry
// and same 20 GeV μ− offset in x; field_null_provider means no deflection.
local lib = import 'lib.libsonnet';
{
  driver: lib.driver(100),
  sources: {
    geometry: lib.builtin_geometry,
    field: lib.null_field,
    gun: lib.pencil_gun { vertex_x: 200.0 },
  },
  modules: {
    geant4: lib.geant4,
    output: lib.full_output('gun_st_nofield_smoke.root', 'gun_st_nofield_smoke_validation.root'),
  },
}
