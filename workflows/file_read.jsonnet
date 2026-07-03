// Read MCParticles from a ROOT RNTuple and run them through Geant4.
// The file must hold a std::vector<SHiP::MCParticle> field "mc_particles" in an
// RNTuple named "events" (as written by sim_output_module or file_write_mc).
//   n=$(pixi run count_entries input.root)   # read the whole file
//   jsonnet --ext-str events=$n --ext-str infile=input.root \
//       --ext-str simout=sim.root --ext-str histo=valid.root file_read.jsonnet
local n_events = std.parseInt(std.extVar('events'));
{
  driver: {
    cpp: 'generate_layers',
    layers: { event: { total: n_events } },
  },
  sources: {
    field: { cpp: 'field_null_provider' },
    geometry: { cpp: 'geometry_builtin_provider' },
    input: {
      cpp: 'file_source',
      input_file: std.extVar('infile'),
      product: 'mc_particles',
      skip: 0,  // start reading at this entry
    },
  },
  modules: {
    geant4: {
      cpp: 'geant4_module',
      physics_list: 'FTFP_BERT',
      verbosity: 0,
    },
    output: {
      cpp: 'sim_output_module',
      mode: 'full',
      rntuple_file: std.extVar('simout'),
      histo_file: std.extVar('histo'),
    },
  },
}
