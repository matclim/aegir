local n_events = std.parseInt(std.extVar('events'));
{
  driver: {
    cpp: 'generate_layers',
    layers: { event: { total: n_events } },
  },
  sources: {
    field: { cpp: 'field_null_provider' },
    geometry: {
      cpp: 'geometry_geomodel_provider',
      db_file: 'ship_geometry.db',
      sensitive_volumes: import 'sensitive_volumes_patterns.jsonnet',
    },
    input: {
      cpp: 'file_particle_source',
      file: '/pnfs/iihe/ship/store/user/mclimesc/Scatter/aegirruns/source/converted_muons_fixed.root',
      ntuple: 'events',
    },
  },
  modules: {
    geant4: { cpp: 'geant4_module', physics_list: 'FTFP_BERT', verbosity: 1 },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      rntuple_file: 'muons_transported_output.root',
      histo_file: 'muons_transported_validation.root',
    },
  },
}
