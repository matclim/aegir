// Produce an RNTuple containing SimParticles (full simulation output) so the
// SimParticle->MCParticle path of file_source can be exercised. Runs the gun
// through Geant4 and writes full output (mc_particles + sim_particles + hits).
//   jsonnet --ext-str events=20 --ext-str outfile=sim_input.root file_write_sim.jsonnet
local n_events = std.parseInt(std.extVar('events'));
{
  driver: {
    cpp: 'generate_layers',
    layers: { event: { total: n_events } },
  },
  sources: {
    field: { cpp: 'field_null_provider' },
    geometry: { cpp: 'geometry_builtin_provider' },
    gun: {
      cpp: 'particle_gun_source',
      pdg: 13,
      p_min: 10.0,
      p_max: 100.0,
      max_theta: 0.1,
      vertex_z: -500.0,
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
      rntuple_file: std.extVar('outfile'),
      histo_file: std.extVar('histofile'),
    },
  },
}
