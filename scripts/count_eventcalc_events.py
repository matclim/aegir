#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

"""Print the number of decay events in an EventCalc-SHiP .dat record.

Lets you drive eventcalc_source over a whole file without knowing the event
count up front: use the printed number as the `events` total of the workflow,
exactly as count_entries.py does for RNTuple input, e.g.

    n=$(pixi run count_eventcalc_events HNL_1.0_0.1_data.dat)
    phlex -c <(jsonnet --ext-str events="$n" --ext-str infile=HNL_1.0_0.1_data.dat \\
        --ext-str simout=sim.root --ext-str histo=valid.root \\
        workflows/eventcalc_st.jsonnet)

The count must match what the C++ reader accepts, so the two skip rules are
kept identical: the leading "Sampled ..." summary line and the
"#<process=...; sample_points=...>" block headers are not events.
"""

import sys

_MIN_COLUMNS = 16  # 10 for the LLP + 6 for at least one decay product


def count(path):
    events = 0
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith(("#", "Sampled")):
                continue
            if len(line.split()) >= _MIN_COLUMNS:
                events += 1
    return events


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <file_data.dat>", file=sys.stderr)
        sys.exit(2)
    print(count(sys.argv[1]))
