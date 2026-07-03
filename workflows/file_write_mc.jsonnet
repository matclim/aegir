// Produce an RNTuple of MCParticles for file_source to read back.
// Runs the particle gun and writes MC-only output (no Geant4).
//   jsonnet --ext-str events=20 --ext-str outfile=input.root file_write_mc.jsonnet
local n_events = std.parseInt(std.extVar('events'));
{
  driver: {
    cpp: 'generate_layers',
    layers: { event: { total: n_events } },
  },
  sources: {
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
    output: {
      cpp: 'sim_output_module',
      mode: 'mc_only',
      rntuple_file: std.extVar('outfile'),
      histo_file: std.extVar('histofile'),
    },
  },
}
