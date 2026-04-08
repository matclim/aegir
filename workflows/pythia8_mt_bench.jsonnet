local num_events = std.parseInt(std.extVar('num_events'));
local concurrency = std.parseInt(std.extVar('concurrency'));
local pythia_threads = std.parseInt(std.extVar('pythia_threads'));

{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: num_events },
    },
  },
  sources: {
    geometry: { cpp: 'geometry_builtin_provider' },
    pythia8: {
      cpp: 'pythia8_source',
      beam_energy: 400.0,
      process: 'SoftQCD:inelastic',
      parallel: true,
      num_threads: pythia_threads,
      num_events: num_events,
    },
  },
  modules: {
    geant4: {
      cpp: 'geant4_module',
      physics_list: 'FTFP_BERT',
      verbosity: 0,
      concurrency: concurrency,
    },
  },
}
