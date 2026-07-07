#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later
"""List the target nuclei of a geometry as ion PDG codes.

Imports a GDML file (e.g. produced with the geant4 module's
`export_gdml` option) and walks all materials, emitting the unique
nuclide PDG codes (10LZZZAAAI convention, e.g. 1000741840 for W-184).
This is the target list a neutrino event generator needs cross-section
splines for: GENIE's geometry driver samples interactions on every
nuclide present in the scanned volume.

Elements defined with explicit isotopes are expanded; natural elements
without isotope tables are emitted with their mass number rounded, which
matches how GENIE's ROOTGeomAnalyzer interprets them.

Usage: gdml_target_nuclei.py geometry.gdml [--gmkspl]
  --gmkspl  print a single comma-separated list suitable for `gmkspl -t`
"""

import argparse
import os
import sys

import ROOT


def ion_pdg(z, a):
    return 1000000000 + z * 10000 + a * 10


def collect_nuclei(geo):
    nuclei = {}
    for material in geo.GetListOfMaterials():
        if material.GetDensity() <= 1e-10:  # vacuum
            continue
        if material.GetNelements() == 0 or not material.GetElement(0):
            # Simple material defined directly by Z/A, no element table.
            z = round(material.GetZ())
            if z >= 1:
                nuclei[ion_pdg(z, round(material.GetA()))] = material.GetName()
            continue
        for i in range(material.GetNelements()):
            element = material.GetElement(i)
            z = round(element.Z())
            if z < 1:
                continue
            if element.GetNisotopes() > 0:
                for j in range(element.GetNisotopes()):
                    isotope = element.GetIsotope(j)
                    if element.GetRelativeAbundance(j) <= 0:
                        continue
                    nuclei[ion_pdg(z, isotope.GetN())] = element.GetName()
            else:
                nuclei[ion_pdg(z, round(element.A()))] = element.GetName()
    return nuclei


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("gdml", help="GDML geometry file")
    parser.add_argument(
        "--gmkspl",
        action="store_true",
        help="print one comma-separated list for gmkspl -t",
    )
    args = parser.parse_args()

    ROOT.gErrorIgnoreLevel = ROOT.kError
    geo = ROOT.TGeoManager.Import(args.gdml)
    if not geo:
        raise SystemExit(f"error: cannot import '{args.gdml}'")

    nuclei = collect_nuclei(geo)
    if not nuclei:
        raise SystemExit("error: no nuclei found — empty geometry?")

    if args.gmkspl:
        print(",".join(str(pdg) for pdg in sorted(nuclei)))
    else:
        for pdg in sorted(nuclei):
            print(f"{pdg}  {nuclei[pdg]}")

    sys.stdout.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
