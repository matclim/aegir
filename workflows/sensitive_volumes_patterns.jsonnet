// Base-name patterns for aegir sensitive_volumes.
// Use this form if detector_construction.hpp matches names by substring/prefix.
// Every token matches ONLY active detector volumes (verified: no passive leakage).
[
  // Straw tracker — gas of every straw (9600)
  'straw_gas',

  // SBT (decay volume) — scintillator sensor cells, both _W# and _C# sub-cells
  'sensors_',

  // Calorimeter — scintillator media only
  'wide_pvt',          // PVT plastic scintillator
  'thin_ps',           // polystyrene scintillator
  'HPL_FiberCoreLog',  // scintillating fibre cores (active core only)

  // Neutrino detector (SND)
  'Si_X', 'Si_Y',      // silicon tracker planes
  'bar_h', 'bar_v',    // veto scintillator bars

  // Upstream tagger
  'fine_tile', 'coarse_tile',

  // Timing detector
  'TimDetBar',
]
