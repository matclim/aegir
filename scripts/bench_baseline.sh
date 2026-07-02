#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# Baseline measurement protocol for the Geant4 integration performance work.
# Run from the repo root inside `pixi shell -e bench`.
#
# Produces, per (g4_threads, -j) cell in results/:
#   baseline_<sha>.json            hyperfine wall-time distributions
#   perfstat_<sha>_g<T>j<J>.txt    perf stat counters (incl. context switches)
#   time_<sha>_g<T>j<J>.txt        /usr/bin/time -v (voluntary/involuntary split)
#   trace_<sha>_g<T>j<J>.json      Chrome trace from the build-trace binary

set -euo pipefail

EVENTS=${EVENTS:-2000}
MATRIX=${MATRIX:-"4 8 12"}
SHA=$(git rev-parse --short HEAD)
OUT=results
mkdir -p "$OUT"

PHLEX_LIB=$(dirname "$(which phlex)")/../lib
WORKFLOW=workflows/gun_mt_bench.jsonnet

cfg() { jsonnet -V num_events="$EVENTS" -V concurrency="$1" "$WORKFLOW" > "$2"; }

run_cell() {
  local t=$1 build=$2
  local cfgfile
  cfgfile=$(mktemp --suffix=.json)
  cfg "$t" "$cfgfile"
  PHLEX_PLUGIN_PATH="$build:$PHLEX_LIB" phlex -c "$cfgfile" -j "$t"
  rm -f "$cfgfile"
}

echo "== hyperfine wall time (build/, trace off) =="
HF_ARGS=(--warmup 1 --min-runs 10 --export-json "$OUT/baseline_${SHA}.json"
         --export-markdown "$OUT/baseline_${SHA}.md")
for t in $MATRIX; do
  cfgfile="$OUT/cfg_${EVENTS}ev_${t}t.json"
  cfg "$t" "$cfgfile"
  HF_ARGS+=(-n "g4=${t} j=${t}"
            "PHLEX_PLUGIN_PATH=build:$PHLEX_LIB phlex -c $cfgfile -j $t")
done
hyperfine "${HF_ARGS[@]}"

echo "== perf stat + time -v (build/, trace off) =="
for t in $MATRIX; do
  cfgfile="$OUT/cfg_${EVENTS}ev_${t}t.json"
  perf stat -e cycles,instructions,context-switches,cpu-migrations,cache-references,cache-misses \
    -o "$OUT/perfstat_${SHA}_g${t}j${t}.txt" -- \
    env PHLEX_PLUGIN_PATH="build:$PHLEX_LIB" phlex -c "$cfgfile" -j "$t" > /dev/null
  command time -v env PHLEX_PLUGIN_PATH="build:$PHLEX_LIB" phlex -c "$cfgfile" -j "$t" \
    > /dev/null 2> "$OUT/time_${SHA}_g${t}j${t}.txt"
done

echo "== chrome traces (build-trace/) =="
for t in $MATRIX; do
  cfgfile="$OUT/cfg_${EVENTS}ev_${t}t.json"
  AEGIR_TRACE_FILE="$OUT/trace_${SHA}_g${t}j${t}.json" \
    PHLEX_PLUGIN_PATH="build-trace:$PHLEX_LIB" phlex -c "$cfgfile" -j "$t" > /dev/null
done

echo "Baseline recorded for $SHA in $OUT/"
