#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# Determinism check: the particle-gun generator seeds a counter-based RNG
# per event, so two runs with the same event count must produce identical
# output. Run the gun workflow twice and compare the validation histograms.
# This exercises the Philox path and guards against RNG regressions.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

cat >"$workdir/gun.jsonnet" <<'EOF'
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
EOF

run() {
  phlex -c <(jsonnet \
    --ext-str events=100 \
    --ext-str outfile="$1" \
    --ext-str histofile="$2" \
    "$workdir/gun.jsonnet")
}

run "$workdir/det1.root" "$workdir/hist1.root"
run "$workdir/det2.root" "$workdir/hist2.root"

python3 "$here/compare_histograms.py" "$workdir/hist1.root" "$workdir/hist2.root"
echo "determinism check passed: identical output across runs"
