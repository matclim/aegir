#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# Exhaustion check: when the driver requests more events than the parallel
# Pythia8 source's configured num_events, Pythia8MTSource::pop() must throw
# rather than emit silently-empty events. Drive generation past num_events and
# assert phlex fails loudly with the exhaustion error. Guards against the source
# ever reverting to empty output.
set -euo pipefail

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

cat >"$workdir/exhaust.jsonnet" <<'EOF'
{
  driver: {
    cpp: 'generate_layers',
    // Request more events than the source can produce.
    layers: { event: { total: 20 } },
  },
  sources: {
    pythia8: {
      cpp: 'pythia8_source',
      beam_energy: 400.0,
      process: 'SoftQCD:inelastic',
      parallel: true,
      num_threads: 2,
      num_events: 10,
    },
  },
  modules: {
    output: {
      cpp: 'sim_output_module',
      mode: 'noop',
    },
  },
}
EOF

if out=$(phlex -c <(jsonnet "$workdir/exhaust.jsonnet") 2>&1); then
  echo "FAIL: expected exhaustion error, but phlex succeeded"
  echo "$out"
  exit 1
fi

if ! grep -q "generation exhausted" <<<"$out"; then
  echo "FAIL: phlex failed but not with the exhaustion error"
  echo "$out"
  exit 1
fi

echo "exhaustion check passed: source throws when driver outpaces num_events"
