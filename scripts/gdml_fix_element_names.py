#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Make Geant4-written GDML safe for ROOT's TGeo importer.

ROOT's TGDMLParse strips the Geant4 pointer suffixes ("Iron0x1234" ->
"Iron") and then mixes up materials and elements that end up with the
same stripped name — a common pattern in Geant4, where the NIST
material "Iron" is built from an element also called "Iron". The
affected materials import as empty mixtures (no elements, Z = 0), which
silently breaks downstream consumers such as GENIE's ROOTGeomAnalyzer
(no target nuclei in exactly those volumes).

This script renames every element whose stripped name collides with a
material's stripped name (appending "_el" before the pointer suffix)
and updates all references, leaving the geometry physics unchanged.

Usage: gdml_fix_element_names.py in.gdml out.gdml
"""

import argparse
import re
import sys

POINTER_SUFFIX = re.compile(r"0x[0-9a-f]+$")


def stripped(name):
    return POINTER_SUFFIX.sub("", name)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("infile")
    parser.add_argument("outfile")
    args = parser.parse_args()

    with open(args.infile, encoding="utf-8") as f:
        gdml = f.read()

    element_names = set(re.findall(r'<element name="([^"]+)"', gdml))
    material_names = set(re.findall(r'<material name="([^"]+)"', gdml))
    material_stripped = {stripped(n) for n in material_names}

    colliding = sorted(n for n in element_names if stripped(n) in material_stripped)
    if not colliding:
        print("no element/material name collisions found")
    for name in colliding:
        if POINTER_SUFFIX.search(name):
            new = POINTER_SUFFIX.sub(lambda m: "_el" + m.group(0), name)
        else:
            new = name + "_el"
        gdml = gdml.replace(f'"{name}"', f'"{new}"')
        print(f"renamed element {name} -> {new}")

    with open(args.outfile, "w", encoding="utf-8") as f:
        f.write(gdml)

    sys.stdout.flush()


if __name__ == "__main__":
    main()
