local n_events = std.parseInt(std.extVar('events'));
{
  driver: {
    cpp: 'generate_layers',
    layers: {
      event: { total: n_events },
    },
  },
  sources: {
    field: { cpp: 'field_null_provider' },
    pythia8: {
      cpp: 'pythia8_source',
      beam_energy: 400.0,
      process: 'SoftQCD:inelastic',
    },
  },
  modules: {
    output: {
      cpp: 'sim_output_module',
      mode: 'mc_only',
      rntuple_file: 'pythia8_only_output.root',
      histo_file: 'pythia8_only_validation.root',
    },
  },
}
