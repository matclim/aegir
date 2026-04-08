#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

"""Benchmark runner for concurrency scaling measurements.

Sweeps over G4 thread counts and phlex parallelism levels, collects
timing data (wall clock, CPU, per-event queue/processing latencies)
and writes results to CSV.
"""

import argparse
import csv
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile


WORKFLOW_JSONNET = {
    "gun_mt": "workflows/gun_mt_bench.jsonnet",
    "pythia8_mt": "workflows/pythia8_mt_bench.jsonnet",
}


def render_jsonnet(workflow, num_events, g4_threads, timing_file, pythia_threads=None):
    """Render a benchmark Jsonnet config to a temporary JSON file."""
    jsonnet_file = WORKFLOW_JSONNET[workflow]
    ext_vars = {
        "num_events": str(num_events),
        "g4_threads": str(g4_threads),
        "timing_file": timing_file,
    }
    if workflow == "pythia8_mt":
        ext_vars["pythia_threads"] = str(
            pythia_threads if pythia_threads is not None else g4_threads
        )

    cmd = ["jsonnet"]
    for k, v in ext_vars.items():
        cmd += ["-V", f"{k}={v}"]
    cmd.append(jsonnet_file)

    result = subprocess.run(cmd, capture_output=True, text=True, check=True)

    tmp = tempfile.NamedTemporaryFile(
        mode="w", suffix=".json", delete=False, prefix="bench_"
    )
    tmp.write(result.stdout)
    tmp.close()
    return tmp.name


def run_phlex(config_json, parallelism):
    """Run phlex with the given config and parallelism level."""
    cmd = ["phlex", "-c", config_json, "-j", str(parallelism)]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    return result


def parse_phlex_output(text):
    """Extract timing/resource metrics from phlex output."""
    metrics = {}

    m = re.search(r"CPU time[:\s]+([\d.]+)\s*s", text)
    if m:
        metrics["cpu_time_s"] = float(m.group(1))

    m = re.search(r"Real time[:\s]+([\d.]+)\s*s", text)
    if m:
        metrics["real_time_s"] = float(m.group(1))

    m = re.search(r"CPU efficiency[:\s]+([\d.]+)\s*%", text)
    if m:
        metrics["cpu_efficiency_pct"] = float(m.group(1))

    m = re.search(r"Max\.?\s*RSS[:\s]+([\d.]+)\s*(MB|kB)", text)
    if m:
        val = float(m.group(1))
        if m.group(2) == "kB":
            val /= 1024.0
        metrics["max_rss_mb"] = val

    return metrics


def parse_timing_csv(timing_file):
    """Parse per-event timing CSV and compute summary statistics."""
    stats = {
        "mean_queue_wait_ms": "",
        "mean_g4_process_ms": "",
        "mean_round_trip_ms": "",
        "median_round_trip_ms": "",
        "p95_round_trip_ms": "",
    }
    if not timing_file or not os.path.exists(timing_file):
        return stats

    queue_waits = []
    g4_procs = []
    round_trips = []

    with open(timing_file) as f:
        reader = csv.DictReader(f)
        for row in reader:
            queue_waits.append(float(row["queue_wait_ms"]))
            g4_procs.append(float(row["g4_process_ms"]))
            round_trips.append(float(row["round_trip_ms"]))

    if not round_trips:
        return stats

    sorted_rt = sorted(round_trips)
    p95_idx = int(len(sorted_rt) * 0.95)

    stats["mean_queue_wait_ms"] = f"{statistics.mean(queue_waits):.3f}"
    stats["mean_g4_process_ms"] = f"{statistics.mean(g4_procs):.3f}"
    stats["mean_round_trip_ms"] = f"{statistics.mean(round_trips):.3f}"
    stats["median_round_trip_ms"] = f"{statistics.median(round_trips):.3f}"
    stats["p95_round_trip_ms"] = f"{sorted_rt[min(p95_idx, len(sorted_rt) - 1)]:.3f}"

    return stats


COLUMNS = [
    "workflow",
    "num_events",
    "g4_threads",
    "parallelism",
    "repeat",
    "return_code",
    "cpu_time_s",
    "real_time_s",
    "cpu_efficiency_pct",
    "max_rss_mb",
    "mean_queue_wait_ms",
    "mean_g4_process_ms",
    "mean_round_trip_ms",
    "median_round_trip_ms",
    "p95_round_trip_ms",
]


def run_sweep(args):
    """Execute the full benchmark sweep."""
    results = []
    total_runs = len(args.g4_threads) * len(args.parallelism) * args.repeats
    run_num = 0

    for g4_t in args.g4_threads:
        for par in args.parallelism:
            for rep in range(1, args.repeats + 1):
                run_num += 1
                print(
                    f"[{run_num}/{total_runs}] "
                    f"g4_threads={g4_t} parallelism={par} repeat={rep}"
                )

                timing_file = tempfile.mktemp(suffix=".csv", prefix="timing_")

                try:
                    config_json = render_jsonnet(
                        args.workflow,
                        args.num_events,
                        g4_t,
                        timing_file,
                        pythia_threads=par if args.workflow == "pythia8_mt" else None,
                    )
                except subprocess.CalledProcessError as e:
                    print(f"  jsonnet failed: {e.stderr}", file=sys.stderr)
                    continue

                try:
                    result = run_phlex(config_json, par)
                    combined_output = result.stdout + result.stderr
                    metrics = parse_phlex_output(combined_output)
                    timing_stats = parse_timing_csv(timing_file)

                    row = {
                        "workflow": args.workflow,
                        "num_events": args.num_events,
                        "g4_threads": g4_t,
                        "parallelism": par,
                        "repeat": rep,
                        "return_code": result.returncode,
                        "cpu_time_s": metrics.get("cpu_time_s", ""),
                        "real_time_s": metrics.get("real_time_s", ""),
                        "cpu_efficiency_pct": metrics.get("cpu_efficiency_pct", ""),
                        "max_rss_mb": metrics.get("max_rss_mb", ""),
                        **timing_stats,
                    }
                    results.append(row)

                    rt = metrics.get("real_time_s", "?")
                    eff = metrics.get("cpu_efficiency_pct", "?")
                    print(f"  real={rt}s  efficiency={eff}%  rc={result.returncode}")

                except subprocess.TimeoutExpired:
                    print("  TIMEOUT", file=sys.stderr)
                    results.append(
                        {
                            "workflow": args.workflow,
                            "num_events": args.num_events,
                            "g4_threads": g4_t,
                            "parallelism": par,
                            "repeat": rep,
                            "return_code": -1,
                        }
                    )
                finally:
                    if os.path.exists(config_json):
                        os.unlink(config_json)
                    if os.path.exists(timing_file):
                        os.unlink(timing_file)
                    # Clean up any ROOT files from the run
                    for f in os.listdir("."):
                        if f.startswith(("gun_mt_", "pythia8_mt_")) and (
                            f.endswith(".root")
                        ):
                            os.unlink(f)

    return results


def write_csv(results, output_path):
    """Write benchmark results to CSV."""
    with open(output_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=COLUMNS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(results)
    print(f"Results written to {output_path}")


def plot_results(results, output_prefix):
    """Generate scaling plots from benchmark results."""
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available, skipping plots", file=sys.stderr)
        return

    # Filter to successful runs
    data = [r for r in results if r.get("return_code") == 0 and r.get("real_time_s")]

    if not data:
        print("No successful runs to plot", file=sys.stderr)
        return

    # Group by (g4_threads, parallelism) — average over repeats
    grouped = {}
    for r in data:
        key = (r["g4_threads"], r["parallelism"])
        grouped.setdefault(key, []).append(r)

    def avg(rows, field):
        vals = [float(r[field]) for r in rows if r.get(field)]
        return statistics.mean(vals) if vals else None

    # 1. Real time vs g4_threads
    fig, axes = plt.subplots(2, 2, figsize=(12, 10))

    par_levels = sorted(set(r["parallelism"] for r in data))
    g4_levels = sorted(set(r["g4_threads"] for r in data))

    for par in par_levels:
        xs, ys = [], []
        for g4_t in g4_levels:
            v = avg(grouped.get((g4_t, par), []), "real_time_s")
            if v is not None:
                xs.append(g4_t)
                ys.append(v)
        if xs:
            axes[0, 0].plot(xs, ys, "o-", label=f"parallelism={par}")
    axes[0, 0].set_xlabel("G4 threads")
    axes[0, 0].set_ylabel("Real time (s)")
    axes[0, 0].set_title("Real time vs G4 threads")
    axes[0, 0].legend()

    # 2. CPU efficiency vs parallelism
    for g4_t in g4_levels:
        xs, ys = [], []
        for par in par_levels:
            v = avg(grouped.get((g4_t, par), []), "cpu_efficiency_pct")
            if v is not None:
                xs.append(par)
                ys.append(v)
        if xs:
            axes[0, 1].plot(xs, ys, "o-", label=f"g4_threads={g4_t}")
    axes[0, 1].set_xlabel("Parallelism")
    axes[0, 1].set_ylabel("CPU efficiency (%)")
    axes[0, 1].set_title("CPU efficiency vs parallelism")
    axes[0, 1].legend()

    # 3. Mean queue wait vs g4_threads
    for par in par_levels:
        xs, ys = [], []
        for g4_t in g4_levels:
            v = avg(grouped.get((g4_t, par), []), "mean_queue_wait_ms")
            if v is not None:
                xs.append(g4_t)
                ys.append(v)
        if xs:
            axes[1, 0].plot(xs, ys, "o-", label=f"parallelism={par}")
    axes[1, 0].set_xlabel("G4 threads")
    axes[1, 0].set_ylabel("Mean queue wait (ms)")
    axes[1, 0].set_title("Queue wait vs G4 threads")
    axes[1, 0].legend()

    # 4. Speedup relative to baseline
    baseline_key = (min(g4_levels), min(par_levels))
    baseline_rt = avg(grouped.get(baseline_key, []), "real_time_s")

    if baseline_rt:
        for par in par_levels:
            xs, ys = [], []
            for g4_t in g4_levels:
                v = avg(grouped.get((g4_t, par), []), "real_time_s")
                if v is not None:
                    xs.append(g4_t)
                    ys.append(baseline_rt / v)
            if xs:
                axes[1, 1].plot(xs, ys, "o-", label=f"parallelism={par}")
        # Ideal scaling line
        axes[1, 1].plot(g4_levels, g4_levels, "k--", alpha=0.3, label="ideal")
    axes[1, 1].set_xlabel("G4 threads")
    axes[1, 1].set_ylabel("Speedup")
    axes[1, 1].set_title("Speedup vs baseline")
    axes[1, 1].legend()

    plt.tight_layout()
    plot_file = output_prefix.rsplit(".", 1)[0] + ".png"
    plt.savefig(plot_file, dpi=150)
    print(f"Plot saved to {plot_file}")


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark runner for concurrency scaling"
    )
    parser.add_argument(
        "--workflow",
        required=True,
        choices=list(WORKFLOW_JSONNET),
        help="Workflow to benchmark",
    )
    parser.add_argument(
        "--num-events",
        type=int,
        default=200,
        help="Number of events per run (default: 200)",
    )
    parser.add_argument(
        "--g4-threads",
        type=int,
        nargs="+",
        default=[1, 2, 4, 8],
        help="G4 worker thread counts to sweep",
    )
    parser.add_argument(
        "--parallelism",
        type=int,
        nargs="+",
        default=[1, 2, 4, 8],
        help="Phlex parallelism levels to sweep",
    )
    parser.add_argument(
        "--repeats",
        type=int,
        default=3,
        help="Number of repeats per configuration (default: 3)",
    )
    parser.add_argument(
        "--output",
        default="benchmark_results.csv",
        help="Output CSV file (default: benchmark_results.csv)",
    )
    parser.add_argument(
        "--plot",
        action="store_true",
        help="Generate scaling plots (requires matplotlib)",
    )
    args = parser.parse_args()

    # Check dependencies
    if not shutil.which("jsonnet"):
        print("Error: jsonnet not found in PATH", file=sys.stderr)
        sys.exit(1)
    if not shutil.which("phlex"):
        print("Error: phlex not found in PATH", file=sys.stderr)
        sys.exit(1)

    results = run_sweep(args)
    write_csv(results, args.output)

    if args.plot:
        plot_results(results, args.output)


if __name__ == "__main__":
    main()
