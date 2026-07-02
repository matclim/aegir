#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

"""Compare validation histograms bin-by-bin between two ROOT files.

Used by the determinism check: two generator runs with the same seed must
produce identical distributions. Histogram bin contents are order-independent,
so this is robust to the multi-threaded writer emitting events in any order.
Exits non-zero on any mismatch.
"""

import sys

import ROOT

HISTOGRAMS = ["h_mc_multiplicity", "h_mc_momentum", "h_mc_pdg"]


def compare(path_a, path_b):
    fa = ROOT.TFile.Open(path_a)
    fb = ROOT.TFile.Open(path_b)
    if not fa or fa.IsZombie() or not fb or fb.IsZombie():
        print(f"could not open {path_a} and/or {path_b}")
        return False

    ok = True
    for name in HISTOGRAMS:
        ha = fa.Get(name)
        hb = fb.Get(name)
        if not ha or not hb:
            print(f"missing histogram {name}")
            ok = False
            continue
        if ha.GetNbinsX() != hb.GetNbinsX():
            print(f"{name}: bin count differs")
            ok = False
            continue
        for b in range(0, ha.GetNbinsX() + 2):  # include under/overflow
            if ha.GetBinContent(b) != hb.GetBinContent(b):
                print(
                    f"{name}: bin {b} differs "
                    f"({ha.GetBinContent(b)} != {hb.GetBinContent(b)})"
                )
                ok = False
                break
    return ok


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <file_a.root> <file_b.root>")
        sys.exit(2)
    sys.exit(0 if compare(sys.argv[1], sys.argv[2]) else 1)
