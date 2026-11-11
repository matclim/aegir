#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# Proton-nucleus collisions (fixed_target with nucleus: true): Pythia's
# heavy-ion model Angantyr, p + 96Mo at 400 GeV, generated in the
# nucleon-nucleon centre-of-mass frame and boosted to the lab. Checks that
#   - the job runs and logs the Angantyr set-up (sqrt(s_NN), beta);
#   - the events are in the lab frame: no final-state particle far backward
#     (in the centre-of-mass frame they reach pz = -13.7 GeV), a leading
#     particle above 30 GeV and none above the beam energy;
#   - no nucleus (|PDG| >= 1e9) is left with status 1 for Geant4.
# Angantyr fits its sub-collision model at initialisation (about a minute).
# Relies on PHLEX_PLUGIN_PATH being set (activate.sh does this under `pixi run`).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
workflows="$here/../workflows"
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

n=5

cat >"$workdir/job.jsonnet" <<'EOF'
local lib = import 'lib.libsonnet';
{
  stage: lib.stage,
  driver: lib.driver(std.parseInt(std.extVar('events'))),
  sources: {
    pythia8: lib.fixed_target {
      target_z: 42,
      target_a: 96,
      target_z_start: 0.0,
      target_z_end: 696.0,
      interaction_length: 150.0,
      nucleus: true,
      seed: 777,
    },
  },
  modules: {
    output: lib.mc_only_output(std.extVar('outfile'), std.extVar('histofile')),
  },
}
EOF

if ! phlex -c <(jsonnet -J "$workflows" --ext-str events="$n" \
  --ext-str outfile="$workdir/pA.root" \
  --ext-str histofile="$workdir/pA_validation.root" "$workdir/job.jsonnet") \
  >"$workdir/pA.log" 2>&1; then
  echo "FAIL: the p-A job failed"
  tail -30 "$workdir/pA.log"
  exit 1
fi
grep -q "with Angantyr, sqrt(s_NN) = 27.4" "$workdir/pA.log" ||
  { echo "FAIL: no Angantyr set-up line in the log"; tail -30 "$workdir/pA.log"; exit 1; }

# os._exit avoids a spurious PyROOT teardown crash.
cat >"$workdir/check.py" <<'EOF'
import os
import sys

import ROOT

ROOT.gInterpreter.Declare('#include "SHiP/MCParticle.hpp"')
reader = ROOT.RNTupleReader.Open("events", sys.argv[1])
view = reader.GetView["std::vector<SHiP::MCParticle>"]("mc_particles")
n_events, problems = 0, []
for i in range(reader.GetNEntries()):
    parts = [p for p in view(i) if p.status == 1]
    n_events += 1
    if not parts:
        problems.append(f"event {i}: no status-1 particles")
        continue
    pz = [p.momentum[2] for p in parts]
    e = [p.energy for p in parts]
    if min(pz) < -2.0:
        problems.append(f"event {i}: pz down to {min(pz):.2f} GeV (not boosted?)")
    if max(e) < 30.0 or max(e) > 400.5:
        problems.append(f"event {i}: leading energy {max(e):.1f} GeV")
    nuclei = [p.pdg_code for p in parts if abs(p.pdg_code) >= 1000000000]
    if nuclei:
        problems.append(f"event {i}: nuclei left for Geant4 {nuclei}")
print(f"{n_events} events checked")
for p in problems:
    print("FAIL:", p)
sys.stdout.flush()
os._exit(1 if problems or n_events == 0 else 0)
EOF
python3 "$workdir/check.py" "$workdir/pA.root"
echo "nucleus_target: OK"
