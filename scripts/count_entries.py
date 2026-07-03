#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

"""Print the number of entries (events) in a ROOT RNTuple.

Lets you drive file_source over a whole file without knowing the event count
up front: use the printed number as the `events` total of the workflow, e.g.

    n=$(pixi run count_entries input.root)
    phlex -c <(jsonnet --ext-str events="$n" --ext-str infile=input.root \\
        workflows/file_read.jsonnet)
"""

import os
import sys

import ROOT


def count(path, ntuple):
    reader = ROOT.RNTupleReader.Open(ntuple, path)
    return reader.GetNEntries()


if __name__ == "__main__":
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print(f"usage: {sys.argv[0]} <file.root> [ntuple_name]", file=sys.stderr)
        sys.exit(2)
    ntuple = sys.argv[2] if len(sys.argv) == 3 else "events"
    print(count(sys.argv[1], ntuple))
    sys.stdout.flush()
    # Skip interpreter shutdown to dodge a PyROOT RNTuple teardown crash.
    os._exit(0)
