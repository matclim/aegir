#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# Scoring planes: run gun_st_scoring_planes with neutrinos and with muons and
# check that every plane records exactly the tracks the geometry says cross
# it, at its entry face, with the scoring-plane detector id; that neutrinos
# are recorded although they deposit nothing; and that the killing plane
# stops what it records. Then check that bad plane configurations fail with a
# clear error instead of running.
# Relies on PHLEX_PLUGIN_PATH being set (activate.sh does this under `pixi run`).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
workflow="$here/../workflows/gun_st_scoring_planes.jsonnet"
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

cat >"$workdir/check_planes.py" <<'EOF'
import math
import os
import sys

import ROOT

path, pdg = sys.argv[1], int(sys.argv[2])
SCORING_PLANE = 100  # SHiP::g4::kScoringPlaneDetectorId
TOL = 1e-3  # mm
# Plane 1: upstream face 10 mm behind the target's end at z = 500 mm.
# Plane 2: tube r = 80-81 mm, |z| <= 500 mm. Plane 3: upstream face at 2999 mm.
Z1, R2, Z3 = 510.0, 80.0, 2999.0
theta_tube = math.atan2(R2, 500.0)  # below: leaves through the tube's end

reader = ROOT.RNTupleReader.Open("events", path)
hits_view = reader.GetView["std::vector<SHiP::SimHit>"]("sim_hits")
parts_view = reader.GetView["std::vector<SHiP::SimParticle>"]("sim_particles")
errors = []
counts = {1: 0, 2: 0, 3: 0}

n = reader.GetNEntries()
for i in range(n):
    primary = next(p for p in parts_view(i) if p.trackId == 1)
    px, py, pz = primary.momentum
    theta = math.atan2(math.hypot(px, py), pz)
    planes = {1: [], 2: [], 3: []}
    detector = []
    for h in hits_view(i):
        if h.detectorId != SCORING_PLANE:
            detector.append(h)
            continue
        if h.geometryNodeId not in planes or h.energyDeposit != 0:
            errors.append(f"event {i}: bad plane hit id={h.geometryNodeId}")
            continue
        counts[h.geometryNodeId] += 1
        if h.trackId == 1:
            planes[h.geometryNodeId].append(h)

    def expect(cond, what):
        if not cond:
            errors.append(f"event {i} (theta={theta:.4f}): {what}")

    # Every primary crosses plane 1 and is killed on plane 3.
    expect(len(planes[1]) == 1, f"{len(planes[1])} hits on plane 1")
    expect(len(planes[3]) == 1, f"{len(planes[3])} hits on plane 3")
    for h in planes[1]:
        expect(abs(h.position[2] - Z1) < TOL, f"plane 1 hit at z={h.position[2]}")
    for h in planes[3]:
        expect(abs(h.position[2] - Z3) < TOL, f"plane 3 hit at z={h.position[2]}")
    end_z = primary.endpoint[2]
    expect(Z3 - TOL < end_z < Z3 + 2.0 + TOL, f"primary ends at z={end_z}")

    if pdg in (12, 14, 16):
        # Straight lines: the tube is crossed exactly when the cone angle
        # takes the neutrino through its wall, entering at r = 80 mm.
        expect(not detector, f"{len(detector)} silicon hits from a neutrino")
        if abs(theta - theta_tube) > 1e-3:
            want = 1 if theta > theta_tube else 0
            expect(len(planes[2]) == want, f"{len(planes[2])} tube hits")
        for h in planes[2]:
            r = math.hypot(h.position[0], h.position[1])
            expect(abs(r - R2) < TOL, f"tube hit at r={r}")
    else:
        # Charged: the silicon at z = 2 m still records it, the killing
        # plane at 3 m stops it before any later silicon plane.
        z_si = [h.position[2] for h in detector if h.trackId == 1]
        expect(any(abs(z - 2000.0) < 1.0 for z in z_si), "no silicon hit at 2 m")
        expect(all(z < Z3 for z in z_si), f"silicon hit behind the kill plane {z_si}")

for e in errors[:20]:
    print(e)
print(f"{n} events, plane hits {counts}, {len(errors)} problem(s)")
ok = n > 0 and not errors and counts[2] > 0
sys.stdout.flush()
os._exit(0 if ok else 1)  # dodge a PyROOT RNTuple teardown crash
EOF

for pdg in 14 13; do
  (cd "$workdir" && phlex -c <(jsonnet --tla-code pdg="$pdg" "$workflow"))
  python3 "$workdir/check_planes.py" "$workdir/gun_st_scoring_planes.root" "$pdg"
  rm -f "$workdir"/gun_st_scoring_planes*.root
done

# Bad configurations: each must stop the job with the given message.
expect_failure() {
  local name=$1 message=$2 planes=$3
  cat >"$workdir/$name.jsonnet" <<EOF
local wf = import '$workflow';
wf(events=2) { modules+: { geant4+: { scoring_planes: $planes } } }
EOF
  local out rc
  out=$(cd "$workdir" && phlex -c <(jsonnet "$workdir/$name.jsonnet") 2>&1) && rc=0 || rc=$?
  if [ "$rc" -eq 0 ] || ! grep -q "$message" <<<"$out"; then
    echo "FAIL: $name: expected the job to fail with '$message' (exit $rc)"
    echo "$out"
    exit 1
  fi
  echo "$name: fails as expected"
}

expect_failure overlap "overlaps the volumes around it" \
  "[{ name: 'p', half_x: 100, half_y: 100, half_z: 5, position: [0, 0, 500] }]"
expect_failure unknown_key "unknown key 'kil'" \
  "[{ name: 'p', half_x: 100, half_y: 100, half_z: 1, position: [0, 0, 1500], kil: true }]"
expect_failure missing_volume "must be placed exactly once" \
  "[{ name: 'p', half_x: 100, half_y: 100, half_z: 1, after_volume: 'NoSuchVolume' }]"
expect_failure two_placements "must be placed exactly once" \
  "[{ name: 'p', half_x: 100, half_y: 100, half_z: 1, after_volume: 'ScoringPlane' }]"

echo "scoring planes passed"
