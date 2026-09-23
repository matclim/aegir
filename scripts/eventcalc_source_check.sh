#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# EventCalc source check: the unit test covers the .dat parser only — it links
# no Phlex, ROOT or Geant4 — so nothing exercises EventCalcSource itself. This
# drives the plugin through Phlex on the test fixture and asserts the three
# behaviours that live in the source rather than in the reader: the whole file
# can be read, running past the end throws the exhaustion error instead of
# emitting empty events, and first_entry offsets into the file.
#
# The config is written here rather than reusing workflows/eventcalc_only.jsonnet
# because first_entry is not an ext-str of that workflow, and because a noop
# output keeps the run to seconds with no Geant4, geometry or field in play.
# Relies on PHLEX_PLUGIN_PATH being set (activate.sh does this under
# `pixi run`), like the other source checks.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fixture="$here/tests/data/eventcalc_sample.dat"

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

# Run the plugin over `events` rows starting at `first_entry`. Echoes phlex's
# combined output and returns its exit status.
run_eventcalc() {
  local events=$1 first_entry=$2
  cat >"$workdir/config.jsonnet" <<EOF
{
  driver: {
    cpp: 'generate_layers',
    layers: { event: { total: ${events} } },
  },
  sources: {
    eventcalc: {
      cpp: 'eventcalc_source',
      file: '${fixture}',
      first_entry: ${first_entry},
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
  phlex -c <(jsonnet "$workdir/config.jsonnet") 2>&1
}

# ---------------------------------------------------------------------------
# 1. The count helper must agree with the parser: the fixture holds 6 decays.
#    If they disagree about what counts as a row, every real job is mis-sized.
# ---------------------------------------------------------------------------
counted=$(python3 "$here/scripts/count_eventcalc_events.py" "$fixture")
if [[ "$counted" != "6" ]]; then
  echo "FAIL: count_eventcalc_events.py reported $counted, expected 6"
  exit 1
fi

# ---------------------------------------------------------------------------
# 2. Reading the whole file must succeed.
# ---------------------------------------------------------------------------
if ! out=$(run_eventcalc 6 0); then
  echo "FAIL: reading all 6 decays failed"
  echo "$out"
  exit 1
fi

# ---------------------------------------------------------------------------
# 3. One past the end must fail loudly. A silently empty event is worse than
#    an error: it pollutes the sample without trace.
# ---------------------------------------------------------------------------
if out=$(run_eventcalc 7 0); then
  echo "FAIL: requesting 7 decays from a 6-decay file succeeded"
  echo "$out"
  exit 1
fi
if ! grep -q "input exhausted" <<<"$out"; then
  echo "FAIL: run failed, but not with the exhaustion error"
  echo "$out"
  exit 1
fi

# ---------------------------------------------------------------------------
# 4. first_entry offsets into the file: the last row alone is readable, and
#    starting one past it is not.
# ---------------------------------------------------------------------------
if ! out=$(run_eventcalc 1 5); then
  echo "FAIL: first_entry=5 should read the last decay"
  echo "$out"
  exit 1
fi
if out=$(run_eventcalc 1 6); then
  echo "FAIL: first_entry=6 is past the end and should have failed"
  echo "$out"
  exit 1
fi

echo "eventcalc source check passed: full read, exhaustion error, first_entry offset"
