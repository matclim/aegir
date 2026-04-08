#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

"""Plot muon momentum distributions from fixed-target simulation output."""

import argparse
import sys

import ROOT
import numpy as np
import matplotlib.pyplot as plt
from matplotlib import rcParams

rcParams.update(
    {
        "text.usetex": True,
        "font.family": "serif",
        "font.size": 14,
        "axes.labelsize": 16,
        "legend.fontsize": 12,
    }
)


def read_muon_data(filename, ntuple_name="events"):
    """Read muon hits from RNTuple output, extracting per-hit momentum."""
    ROOT.gInterpreter.Declare("""
    #include <SHiP/SimHit.hpp>
    #include <cmath>

    struct MuonHit { double p; double pt; int charge; };

    ROOT::RVec<MuonHit> extract_muons(const ROOT::RVec<SHiP::SimHit>& hits) {
        ROOT::RVec<MuonHit> out;
        for (auto const& h : hits) {
            if (std::abs(h.pdgCode) != 13) continue;
            double px = h.momentum[0], py = h.momentum[1], pz = h.momentum[2];
            double p = std::sqrt(px*px + py*py + pz*pz);
            double pt = std::sqrt(px*px + py*py);
            int charge = (h.pdgCode == 13) ? -1 : +1;
            out.push_back({p, pt, charge});
        }
        return out;
    }
    """)

    rdf = ROOT.RDF.FromRNTuple(ntuple_name, filename)
    rdf = rdf.Define("muons", "extract_muons(sim_hits)")
    rdf = rdf.Define(
        "muon_p", "ROOT::RVec<double> v; for(auto& m:muons) v.push_back(m.p); return v;"
    )
    rdf = rdf.Define(
        "muon_pt",
        "ROOT::RVec<double> v; for(auto& m:muons) v.push_back(m.pt); return v;",
    )
    rdf = rdf.Define(
        "muon_charge",
        "ROOT::RVec<int> v; for(auto& m:muons) v.push_back(m.charge); return v;",
    )

    # Collect all values
    all_p = np.asarray(
        rdf.Define("flat_p", "muon_p").Take["ROOT::RVec<double>"]("flat_p").GetValue()
    )
    all_pt = np.asarray(
        rdf.Define("flat_pt", "muon_pt")
        .Take["ROOT::RVec<double>"]("flat_pt")
        .GetValue()
    )
    all_charge = np.asarray(
        rdf.Define("flat_q", "muon_charge").Take["ROOT::RVec<int>"]("flat_q").GetValue()
    )

    # Flatten RVecs
    p_vals = np.concatenate(all_p) if len(all_p) > 0 else np.array([])
    pt_vals = np.concatenate(all_pt) if len(all_pt) > 0 else np.array([])
    q_vals = (
        np.concatenate(all_charge) if len(all_charge) > 0 else np.array([], dtype=int)
    )

    mu_plus = {"p": p_vals[q_vals == 1], "pt": pt_vals[q_vals == 1]}
    mu_minus = {"p": p_vals[q_vals == -1], "pt": pt_vals[q_vals == -1]}
    return mu_plus, mu_minus


def read_muon_data_loop(filename, ntuple_name="events"):
    """Fallback: loop over entries using RNTupleReader."""
    reader = ROOT.RNTupleReader.Open(ntuple_name, filename)
    n = reader.GetNEntries()

    mu_plus_p, mu_plus_pt = [], []
    mu_minus_p, mu_minus_pt = [], []

    for i in range(n):
        reader.LoadEntry(i)
        hits = (
            reader.GetModel()
            .GetDefaultEntry()
            .GetPtr["std::vector<SHiP::SimHit>"]("sim_hits")
        )
        for hit in hits:
            if abs(hit.pdgCode) != 13:
                continue
            px, py, pz = hit.momentum[0], hit.momentum[1], hit.momentum[2]
            p = np.sqrt(px**2 + py**2 + pz**2)
            pt = np.sqrt(px**2 + py**2)
            if hit.pdgCode == 13:
                mu_minus_p.append(p)
                mu_minus_pt.append(pt)
            else:
                mu_plus_p.append(p)
                mu_plus_pt.append(pt)

    mu_plus = {"p": np.array(mu_plus_p), "pt": np.array(mu_plus_pt)}
    mu_minus = {"p": np.array(mu_minus_p), "pt": np.array(mu_minus_pt)}
    return mu_plus, mu_minus


def plot(mu_plus, mu_minus, output, n_pot, e_cut):
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

    bins_p = np.linspace(0, 400, 80)
    bins_pt = np.linspace(0, 10, 60)

    ax1.hist(
        mu_minus["p"],
        bins=bins_p,
        histtype="step",
        linewidth=1.5,
        color="C0",
        label=r"$\mu^-$",
    )
    ax1.hist(
        mu_plus["p"],
        bins=bins_p,
        histtype="step",
        linewidth=1.5,
        color="C3",
        label=r"$\mu^+$",
    )
    ax1.set_xlabel(r"$|p|$ [GeV/$c$]")
    ax1.set_ylabel("Entries / 5 GeV")
    ax1.set_yscale("log")
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    ax2.hist(
        mu_minus["pt"],
        bins=bins_pt,
        histtype="step",
        linewidth=1.5,
        color="C0",
        label=r"$\mu^-$",
    )
    ax2.hist(
        mu_plus["pt"],
        bins=bins_pt,
        histtype="step",
        linewidth=1.5,
        color="C3",
        label=r"$\mu^+$",
    )
    ax2.set_xlabel(r"$p_{\mathrm{T}}$ [GeV/$c$]")
    ax2.set_ylabel(r"Entries / {:.0f} MeV".format(1000 * (bins_pt[1] - bins_pt[0])))
    ax2.set_yscale("log")
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    fig.suptitle(
        rf"Muon momentum at scoring plane ({n_pot} PoT, $E_{{\mathrm{{cut}}}}={e_cut}$ GeV)",
        fontsize=15,
    )
    fig.tight_layout()
    fig.savefig(output, bbox_inches="tight")
    print(f"Saved {output}")
    print(f"  mu-: {len(mu_minus['p'])} hits, mu+: {len(mu_plus['p'])} hits")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="RNTuple ROOT file from simulation")
    parser.add_argument(
        "-o",
        "--output",
        default="muon_momentum.pdf",
        help="Output plot file (default: muon_momentum.pdf)",
    )
    parser.add_argument(
        "-n", "--ntuple", default="events", help="RNTuple name (default: events)"
    )
    parser.add_argument(
        "--pot", default="100k", help="Number of PoT for title (default: 100k)"
    )
    parser.add_argument(
        "--ecut",
        type=float,
        default=50,
        help="Energy cut value for title in GeV (default: 50)",
    )
    args = parser.parse_args()

    try:
        mu_plus, mu_minus = read_muon_data(args.input, args.ntuple)
    except Exception as e:
        print(f"RDataFrame approach failed ({e}), trying loop...", file=sys.stderr)
        mu_plus, mu_minus = read_muon_data_loop(args.input, args.ntuple)

    if len(mu_plus["p"]) == 0 and len(mu_minus["p"]) == 0:
        print("No muon hits found!", file=sys.stderr)
        sys.exit(1)

    plot(mu_plus, mu_minus, args.output, args.pot, args.ecut)


if __name__ == "__main__":
    main()
