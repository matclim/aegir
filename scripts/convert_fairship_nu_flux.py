#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Convert FairShip production files to neutrino flux ntuples.

Reads the neutrino tracks from FairShip `cbmsim` trees (the same
information `muonShieldOptimization/extractNeutrinosAndUpdateWeight.py`
fills into histograms) and writes flux ntuples following the schema in
docs/neutrino_flux.md (version 1).

The cbmsim branches are read leaf-by-leaf, so no FairShip libraries or
dictionaries are needed — any environment with a recent ROOT (>= 6.40,
for the RNTuple API) works, e.g. `pixi run python` in aegir.

Two modes:

  convert   one production sample -> one flux file
            (weights taken from MCTrack.fW, POT given on the command line)
  merge     several flux files -> one, rescaling weights to a common POT
            per the normalisation contract in the schema

Draft status: coordinate frame and time conventions are passed through
unchanged from FairShip (cm -> mm applied); confirm frame offsets and
fStartT units with the neutrino group before production use.
"""

import argparse
import sys

import ROOT

NEUTRINOS = frozenset((12, 14, 16))
# Parents whose neutrinos come from the dedicated heavy-flavour
# productions; excluded from min. bias samples to avoid double counting
# (same list as FairShip's extractNeutrinosAndUpdateWeight.py).
CHARM_PARENTS = frozenset((4332, 4232, 4132, 4122, 431, 411, 421, 15))
PROCESS_IDS = {"mbias": 0, "charm": 1, "beauty": 2}
SCHEMA_VERSION = 1

FLUX_FIELDS = (
    ("pdg", "std::int32_t"),
    ("vx", "double"),
    ("vy", "double"),
    ("vz", "double"),
    ("t", "double"),
    ("px", "double"),
    ("py", "double"),
    ("pz", "double"),
    ("weight", "double"),
    ("parent_pdg", "std::int32_t"),
    ("parent_px", "double"),
    ("parent_py", "double"),
    ("parent_pz", "double"),
    ("process_id", "std::int32_t"),
    ("origin_run", "std::int64_t"),
    ("origin_event", "std::int64_t"),
)

META_FIELDS = (
    ("schema_version", "std::int32_t"),
    ("pot", "double"),
    ("max_energy", "double"),
    ("description", "std::string"),
    ("software", "std::string"),
)


def make_writer(fields, name, target):
    model = ROOT.RNTupleModel.Create()
    for field_name, field_type in fields:
        model.MakeField[field_type](field_name)
    if isinstance(target, str):
        return ROOT.RNTupleWriter.Recreate(model, name, target)
    return ROOT.RNTupleWriter.Append(model, name, target)


def write_meta(out_file_name, pot, max_energy, description, software):
    f = ROOT.TFile.Open(out_file_name, "UPDATE")
    writer = make_writer(META_FIELDS, "flux_meta", f)
    entry = writer.CreateEntry()
    entry["schema_version"] = SCHEMA_VERSION
    entry["pot"] = pot
    entry["max_energy"] = max_energy
    entry["description"] = description
    entry["software"] = software
    writer.Fill(entry)
    del writer
    f.Close()


class CbmsimLeaves:
    """Leaf-based access to one cbmsim entry (no dictionaries needed)."""

    LEAVES = {
        "pdg": "MCTrack.fPdgCode",
        "mother": "MCTrack.fMotherId",
        "px": "MCTrack.fPx",
        "py": "MCTrack.fPy",
        "pz": "MCTrack.fPz",
        "x": "MCTrack.fStartX",
        "y": "MCTrack.fStartY",
        "z": "MCTrack.fStartZ",
        "t": "MCTrack.fStartT",
        "w": "MCTrack.fW",
        "run": "MCEventHeader.fRunId",
        "event": "MCEventHeader.fEventId",
    }

    def __init__(self, tree, points_branch=None):
        self.tree = tree
        # Read only what we use: the hit collections dominate the file
        # size, and skipping them makes streaming from EOS practical.
        tree.SetBranchStatus("*", False)
        tree.SetBranchStatus("MCTrack*", True)
        tree.SetBranchStatus("MCEventHeader*", True)
        if points_branch:
            tree.SetBranchStatus(f"{points_branch}*", True)
        self.leaf = {}
        for key, name in self.LEAVES.items():
            leaf = tree.GetLeaf(name)
            if not leaf:
                raise SystemExit(f"error: leaf '{name}' not found — not a cbmsim tree?")
            self.leaf[key] = leaf
        self.points_track_id = None
        if points_branch:
            self.points_track_id = tree.GetLeaf(f"{points_branch}.fTrackID")
            if not self.points_track_id:
                raise SystemExit(
                    f"error: no '{points_branch}.fTrackID' leaf — wrong --selection branch?"
                )

    def n_tracks(self):
        return self.leaf["pdg"].GetLen()

    def track_ids_to_consider(self):
        """All track indices, or only those with a scoring-plane point."""
        if self.points_track_id is None:
            return range(self.n_tracks())
        n = self.points_track_id.GetLen()
        return sorted({int(self.points_track_id.GetValue(i)) for i in range(n)})

    def value(self, key, i):
        return self.leaf[key].GetValue(i)


def convert(args):
    process_id = PROCESS_IDS[args.process]
    exclude_charm = args.process == "mbias" and not args.keep_charm
    time_to_ns = 1e9 if args.time_unit == "s" else 1.0

    writer = make_writer(FLUX_FIELDS, "nu_flux", args.output)
    entry = writer.CreateEntry()
    n_written = 0
    weight_sum = 0.0
    max_energy = 0.0

    for input_name in args.inputs:
        f = ROOT.TFile.Open(input_name)
        tree = f.Get("cbmsim")
        if not tree:
            raise SystemExit(f"error: no cbmsim tree in {input_name}")
        branch = None
        if args.selection.startswith("points:"):
            branch = args.selection.split(":", 1)[1]
        leaves = CbmsimLeaves(tree, branch)

        for i_entry in range(tree.GetEntries()):
            tree.GetEntry(i_entry)
            n = leaves.n_tracks()
            for i in leaves.track_ids_to_consider():
                if i < 0 or i >= n:
                    continue
                pdg = int(leaves.value("pdg", i))
                if abs(pdg) not in NEUTRINOS:
                    continue
                mother = int(leaves.value("mother", i))
                parent_pdg = int(leaves.value("pdg", mother)) if 0 <= mother < n else 0
                if exclude_charm and abs(parent_pdg) in CHARM_PARENTS:
                    continue

                entry["pdg"] = pdg
                entry["vx"] = leaves.value("x", i) * 10.0  # cm -> mm
                entry["vy"] = leaves.value("y", i) * 10.0
                entry["vz"] = leaves.value("z", i) * 10.0
                entry["t"] = leaves.value("t", i) * time_to_ns
                px, py, pz = (leaves.value(k, i) for k in ("px", "py", "pz"))
                entry["px"], entry["py"], entry["pz"] = px, py, pz
                weight = leaves.value("w", i)
                entry["weight"] = weight
                entry["parent_pdg"] = parent_pdg
                if 0 <= mother < n:
                    entry["parent_px"] = leaves.value("px", mother)
                    entry["parent_py"] = leaves.value("py", mother)
                    entry["parent_pz"] = leaves.value("pz", mother)
                else:
                    entry["parent_px"] = entry["parent_py"] = entry["parent_pz"] = 0.0
                entry["process_id"] = process_id
                entry["origin_run"] = int(leaves.value("run", 0))
                entry["origin_event"] = int(leaves.value("event", 0))
                writer.Fill(entry)

                n_written += 1
                weight_sum += weight
                max_energy = max(max_energy, (px * px + py * py + pz * pz) ** 0.5)
        f.Close()

    del writer
    description = args.description or (
        f"{args.process} sample from {len(args.inputs)} file(s), selection={args.selection}"
    )
    write_meta(
        args.output,
        args.pot,
        max_energy,
        description,
        f"convert_fairship_nu_flux.py schema v{SCHEMA_VERSION}",
    )
    print(
        f"{args.output}: {n_written} neutrinos, sum(weight)={weight_sum:.4g}, pot={args.pot:.4g}, max E={max_energy:.1f} GeV"
    )


def merge(args):
    writer = make_writer(FLUX_FIELDS, "nu_flux", args.output)
    entry = writer.CreateEntry()
    n_written = 0
    max_energy = 0.0
    descriptions = []

    for input_name in args.inputs:
        meta_reader = ROOT.RNTupleReader.Open("flux_meta", input_name)
        meta = meta_reader.CreateEntry()
        meta_reader.LoadEntry(0, meta)
        if meta["schema_version"] != SCHEMA_VERSION:
            raise SystemExit(
                f"error: {input_name} has schema v{meta['schema_version']}, expected v{SCHEMA_VERSION}"
            )
        file_pot = meta["pot"]
        if file_pot <= 0:
            raise SystemExit(
                f"error: {input_name} reports non-positive pot={file_pot!r}, cannot rescale"
            )
        scale = args.pot / file_pot
        max_energy = max(max_energy, meta["max_energy"])
        descriptions.append(f"{input_name} (pot={file_pot:.4g}, weight x{scale:.4g})")

        reader = ROOT.RNTupleReader.Open("nu_flux", input_name)
        in_entry = reader.CreateEntry()
        for i in range(reader.GetNEntries()):
            reader.LoadEntry(i, in_entry)
            for field_name, _ in FLUX_FIELDS:
                entry[field_name] = in_entry[field_name]
            entry["weight"] = in_entry["weight"] * scale
            writer.Fill(entry)
            n_written += 1

    del writer
    write_meta(
        args.output,
        args.pot,
        max_energy,
        "merged: " + "; ".join(descriptions),
        f"convert_fairship_nu_flux.py schema v{SCHEMA_VERSION}",
    )
    print(
        f"{args.output}: {n_written} neutrinos merged from {len(args.inputs)} file(s) at pot={args.pot:.4g}"
    )


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_convert = sub.add_parser(
        "convert", help="convert cbmsim file(s) of one sample to a flux ntuple"
    )
    p_convert.add_argument(
        "inputs", nargs="+", help="FairShip cbmsim ROOT files (local or xrootd)"
    )
    p_convert.add_argument("-o", "--output", required=True)
    p_convert.add_argument(
        "--pot",
        type=float,
        required=True,
        help="protons-on-target equivalent of the inputs combined",
    )
    p_convert.add_argument("--process", choices=sorted(PROCESS_IDS), required=True)
    p_convert.add_argument(
        "--selection",
        default="mctrack",
        help="'mctrack' (all neutrino tracks, default) or 'points:<branch>' (only neutrinos with a point in that scoring branch, e.g. points:vetoPoint)",
    )
    p_convert.add_argument(
        "--keep-charm",
        action="store_true",
        help="keep neutrinos from charm/beauty/tau parents in mbias samples (default: excluded, they come from the dedicated productions)",
    )
    p_convert.add_argument(
        "--time-unit",
        choices=("ns", "s"),
        default="ns",
        help="unit of MCTrack.fStartT in the input (default ns)",
    )
    p_convert.add_argument("--description", default=None)
    p_convert.set_defaults(func=convert)

    p_merge = sub.add_parser(
        "merge", help="merge flux files, rescaling weights to a common POT"
    )
    p_merge.add_argument("inputs", nargs="+", help="flux ntuple files (schema v1)")
    p_merge.add_argument("-o", "--output", required=True)
    p_merge.add_argument(
        "--pot", type=float, required=True, help="POT reference of the merged file"
    )
    p_merge.set_defaults(func=merge)

    args = parser.parse_args()
    ROOT.gErrorIgnoreLevel = ROOT.kError
    args.func(args)
    sys.stdout.flush()


if __name__ == "__main__":
    main()
