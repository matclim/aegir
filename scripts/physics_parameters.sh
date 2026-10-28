#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# Physics parameters end to end (docs/physics_parameters.md):
#   - a parameter point (Pythia8 settings, Geant4 hadronic parameters, an FTF
#     developer parameter and UI commands) runs, reaches Pythia8 (its changed-
#     settings list in the log) and Geant4, and lib.with_run_info stores the
#     whole configuration in the output file as the TNamed run_info;
#   - a misspelt Pythia8 setting, a Random: setting, an unknown Geant4 command
#     and an unknown developer parameter each fail the job with a clear error
#     instead of being ignored or aborting the process;
#   - lib.tune_part rejects a misspelt key of a parameter point.
# Relies on PHLEX_PLUGIN_PATH being set (activate.sh does this under `pixi run`).
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
workflows="$here/../workflows"
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

fail() {
  echo "FAIL: $1"
  [ -n "${2:-}" ] && echo "$2"
  exit 1
}

cat >"$workdir/job.jsonnet" <<'EOF'
local lib = import 'lib.libsonnet';
local tune = std.parseJson(std.extVar('tune'));
lib.with_run_info({
  stage: lib.stage,
  driver: lib.driver(2),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    pythia8: lib.pythia8 { seed: 4242 } + lib.tune_part(tune, 'pythia'),
  },
  modules: {
    geant4: lib.geant4 { seed: 4242 } + lib.tune_part(tune, 'geant4'),
    output: lib.full_output(std.extVar('outfile'), std.extVar('histofile')),
  },
})
EOF

# run <tune JSON> <name>: runs the job, leaves the log in $workdir/<name>.log
run() {
  phlex -c <(jsonnet -J "$workflows" --ext-str tune="$1" \
    --ext-str outfile="$workdir/$2.root" \
    --ext-str histofile="$workdir/$2_validation.root" "$workdir/job.jsonnet") \
    >"$workdir/$2.log" 2>&1
}

# ── a valid parameter point ─────────────────────────────────────────────────
good='{
  "pythia": {"pythia_settings": ["StringFlav:probStoUD = 0.25", "! a comment"]},
  "geant4": {
    "hadronic_flags": {"apply_factor_xs": true},
    "hadronic_parameters": {"xs_factor_pion_inelastic": 1.05},
    "developer_parameters": {"FTF_PION_AVRG_PT2": 0.25},
    "commands": ["/physics_lists/em/GammaToMuons true"]
  }
}'
run "$good" good || fail "valid parameter point: phlex failed" "$(cat "$workdir/good.log")"
grep -q "StringFlav:probStoUD *= *0.25" "$workdir/good.log" ||
  fail "Pythia8's changed settings do not list StringFlav:probStoUD = 0.25" "$(cat "$workdir/good.log")"
grep -q "command '/physics_lists/em/GammaToMuons true'" "$workdir/good.log" ||
  fail "the Geant4 command is not logged" "$(cat "$workdir/good.log")"

python3 - "$workdir/good.root" <<'EOF' || fail "run_info missing or incomplete in the output"
import json
import sys

import ROOT

f = ROOT.TFile.Open(sys.argv[1])
info = f.Get("run_info")
if not info:
    sys.exit("no run_info in " + sys.argv[1])
cfg = json.loads(info.GetTitle())
assert cfg["sources"]["pythia8"]["pythia_settings"][0] == "StringFlav:probStoUD = 0.25", cfg
assert cfg["modules"]["geant4"]["hadronic_parameters"]["xs_factor_pion_inelastic"] == 1.05, cfg
assert cfg["modules"]["geant4"]["seed"] == 4242, cfg
reader = ROOT.RNTupleReader.Open("events", sys.argv[1])
assert reader.GetNEntries() == 2, reader.GetNEntries()
EOF

# ── configuration errors fail cleanly ───────────────────────────────────────
# expect_failure <tune JSON> <name> <text the error must contain>
expect_failure() {
  run "$1" "$2"
  local rc=$?
  [ "$rc" -ne 0 ] ||
    fail "$2: expected a failure, but phlex succeeded" "$(cat "$workdir/$2.log")"
  grep -qF -- "$3" "$workdir/$2.log" ||
    fail "$2: failed without the expected message '$3'" "$(cat "$workdir/$2.log")"
  # A Geant4 abort (G4Exception: SIGABRT) ends with exit 134.
  [ "$rc" -ne 134 ] || fail "$2: the process aborted instead of failing cleanly"
}

expect_failure '{"pythia": {"pythia_settings": ["StringFlav:probStoUDX = 0.2"]}}' \
  bad_pythia "Pythia8 rejected the setting 'StringFlav:probStoUDX = 0.2'"
expect_failure '{"pythia": {"pythia_settings": ["Random:seed = 7"]}}' \
  random_pythia "aegir sets the random numbers itself"
expect_failure '{"geant4": {"commands": ["/physics_lists/em/NoSuchThing true"]}}' \
  bad_command "command not found"
expect_failure '{"geant4": {"developer_parameters": {"FTF_NO_SUCH": 1.0}}}' \
  bad_developer "does not exist"
expect_failure '{"geant4": {"hadronic_parameters": {"xs_factor_pion_inelastic": 1.05}}}' \
  factor_without_flag "apply_factor_xs"

# ── a misspelt key of a parameter point ─────────────────────────────────────
out=$(jsonnet -J "$workflows" --ext-str tune='{"geant4": {"comands": []}}' \
  --ext-str outfile=x --ext-str histofile=y "$workdir/job.jsonnet" 2>&1) &&
  fail "lib.tune_part accepted the misspelt key 'comands'" "$out"
grep -q "unknown geant4 key(s) comands" <<<"$out" ||
  fail "lib.tune_part failed without naming the misspelt key" "$out"

echo "physics-parameters check passed: settings reach Pythia8 and Geant4, run_info stored, bad settings fail cleanly"
