#!/usr/bin/env bash
# Quick smoke test for CI: exercise the standalone Pythia8 binary and
# Phlex plugin loading via the gun_noop workflow (no Geant4 needed).
set -euo pipefail

./build/pythia8_benchmark --events 10

phlex -c <(jsonnet --ext-str events=2 workflows/gun_noop.jsonnet)
