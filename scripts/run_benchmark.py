#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

"""Benchmark runner for concurrency scaling measurements.

Sweeps over G4 thread counts and phlex parallelism levels, collects
timing data (wall clock, CPU, per-event spans from the Chrome trace)
and writes results to CSV.

Per-event percentiles are derived from the Chrome Trace Event JSON
written by aegir when built with -DAEGIR_ENABLE_TRACE=ON. If the trace
file is absent (build without tracing, or the run crashed before
flushing), the per-event columns are left blank.
"""

import argparse
import csv
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


WORKFLOW_JSONNET = {
    "gun_mt": "workflows/gun_mt_bench.jsonnet",
    "pythia8_mt": "workflows/pythia8_mt_bench.jsonnet",
    "fixed_target_mt": "workflows/fixed_target_mt_bench.jsonnet",
}


def render_jsonnet(workflow, num_events, g4_threads, pythia_threads=None):
    """Render a benchmark Jsonnet config to a temporary JSON file."""
    jsonnet_file = WORKFLOW_JSONNET[workflow]
    ext_vars = {
        "num_events": str(num_events),
        "concurrency": str(g4_threads),
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


def run_phlex(config_json, parallelism, trace_file=None):
    """Run phlex with the given config and parallelism level."""
    cmd = ["phlex", "-c", config_json, "-j", str(parallelism)]
    env = os.environ.copy()
    if trace_file:
        env["AEGIR_TRACE_FILE"] = trace_file
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=3600, env=env)
    return result


def affinity_pool():
    """Return the sorted list of CPU IDs this process is allowed to use.

    Respects cgroup/taskset limits — preferable to os.cpu_count() in
    containerised or pinned environments.
    """
    return sorted(os.sched_getaffinity(0))


def run_phlex_cohort(config_json, parallelism, g4_threads, n_copies, cpu_pool):
    """Launch n_copies phlex processes in parallel, each pinned to a
    disjoint g4_threads-wide CPU slice.

    Returns (results, wall_time_s) where results is a list of
    (returncode, combined_output, trace_file_path) tuples.
    """
    procs = []
    base_env = os.environ.copy()
    for i in range(n_copies):
        cpus = cpu_pool[i * g4_threads : (i + 1) * g4_threads]
        if not cpus:
            break
        cpu_list = ",".join(str(c) for c in cpus)
        out_dir = tempfile.mkdtemp(prefix=f"cohort_p{i}_")
        trace_file = os.path.join(out_dir, "trace.json")
        env = dict(base_env)
        env["AEGIR_TRACE_FILE"] = trace_file
        cmd = [
            "taskset",
            "-c",
            cpu_list,
            "phlex",
            "-c",
            config_json,
            "-j",
            str(parallelism),
        ]
        proc = subprocess.Popen(
            cmd,
            cwd=out_dir,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        procs.append((proc, out_dir, trace_file))

    start = time.monotonic()
    results = []
    for proc, out_dir, trace_file in procs:
        try:
            stdout, stderr = proc.communicate(timeout=3600)
            results.append((proc.returncode, stdout + stderr, trace_file, out_dir))
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            results.append((-1, "", trace_file, out_dir))
    wall_time_s = time.monotonic() - start
    return results, wall_time_s


def aggregate_cohort(cohort_results):
    """Average parsed-output metrics and merge trace stats across cohort copies."""
    parsed = [parse_phlex_output(out) for _, out, _, _ in cohort_results]
    valid = [p for p in parsed if "real_time_s" in p]
    if not valid:
        return {
            "cpu_time_s": "",
            "real_time_s": "",
            "cpu_efficiency_pct": "",
            "max_rss_mb": "",
        }

    def mean_or(key):
        vs = [p.get(key) for p in valid if p.get(key) is not None]
        return f"{statistics.mean(vs):.3f}" if vs else ""

    return {
        "cpu_time_s": mean_or("cpu_time_s"),
        "real_time_s": mean_or("real_time_s"),
        "cpu_efficiency_pct": mean_or("cpu_efficiency_pct"),
        "max_rss_mb": mean_or("max_rss_mb"),
    }


def aggregate_cohort_traces(cohort_results):
    """Concatenate per-event spans from every cohort trace and reduce.

    Returns the same column set as parse_chrome_trace, but with
    percentiles computed over the union of events from every cohort
    member.
    """
    all_events = []
    for _, _, trace_file, _ in cohort_results:
        if not os.path.exists(trace_file):
            continue
        try:
            with open(trace_file) as f:
                all_events.extend(json.load(f))
        except (OSError, json.JSONDecodeError):
            continue
    if not all_events:
        return parse_chrome_trace("/nonexistent")
    tmp = tempfile.NamedTemporaryFile(
        mode="w", suffix=".json", delete=False, prefix="cohort_merged_"
    )
    json.dump(all_events, tmp)
    tmp.close()
    try:
        return parse_chrome_trace(tmp.name)
    finally:
        os.unlink(tmp.name)


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


PER_EVENT_SPANS = ("simulate", "ProcessOneEvent", "flush_hits", "build_primaries")


def parse_chrome_trace(trace_file):
    """Aggregate per-event span durations from a Chrome Trace Event JSON.

    Reads complete events (ph='X') in the 'g4' category and returns
    mean/median/p95 of each span's duration in milliseconds, plus the
    derived framework overhead per event (simulate - ProcessOneEvent).
    """
    blank = {
        "mean_simulate_ms": "",
        "p50_simulate_ms": "",
        "p95_simulate_ms": "",
        "mean_process_ms": "",
        "p50_process_ms": "",
        "p95_process_ms": "",
        "mean_overhead_ms": "",
    }
    if not trace_file or not os.path.exists(trace_file):
        return blank

    try:
        with open(trace_file) as f:
            trace = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"  trace parse failed: {e}", file=sys.stderr)
        return blank

    durs_us = {name: [] for name in PER_EVENT_SPANS}
    for ev in trace:
        if ev.get("ph") != "X" or ev.get("cat") != "g4":
            continue
        name = ev.get("name")
        if name in durs_us:
            durs_us[name].append(ev["dur"])

    if not durs_us["simulate"]:
        return blank

    def pct(xs, p):
        if not xs:
            return None
        xs = sorted(xs)
        i = min(int(len(xs) * p), len(xs) - 1)
        return xs[i]

    out = {}
    sim_us = durs_us["simulate"]
    proc_us = durs_us["ProcessOneEvent"]
    out["mean_simulate_ms"] = f"{statistics.mean(sim_us) / 1e3:.3f}"
    out["p50_simulate_ms"] = f"{pct(sim_us, 0.50) / 1e3:.3f}"
    out["p95_simulate_ms"] = f"{pct(sim_us, 0.95) / 1e3:.3f}"
    if proc_us:
        out["mean_process_ms"] = f"{statistics.mean(proc_us) / 1e3:.3f}"
        out["p50_process_ms"] = f"{pct(proc_us, 0.50) / 1e3:.3f}"
        out["p95_process_ms"] = f"{pct(proc_us, 0.95) / 1e3:.3f}"
        out["mean_overhead_ms"] = (
            f"{(statistics.mean(sim_us) - statistics.mean(proc_us)) / 1e3:.3f}"
        )
    else:
        out["mean_process_ms"] = ""
        out["p50_process_ms"] = ""
        out["p95_process_ms"] = ""
        out["mean_overhead_ms"] = ""
    return out


COLUMNS = [
    "workflow",
    "num_events",
    "g4_threads",
    "parallelism",
    "copies",
    "repeat",
    "return_code",
    "cpu_time_s",
    "real_time_s",
    "cohort_wall_s",
    "cpu_efficiency_pct",
    "max_rss_mb",
    "mean_simulate_ms",
    "p50_simulate_ms",
    "p95_simulate_ms",
    "mean_process_ms",
    "p50_process_ms",
    "p95_process_ms",
    "mean_overhead_ms",
]


def run_sweep(args):
    """Execute the full benchmark sweep."""
    results = []
    total_runs = len(args.g4_threads) * len(args.parallelism) * args.repeats
    run_num = 0
    cpu_pool = affinity_pool() if args.saturate else []
    num_cores = len(cpu_pool)

    for g4_t in args.g4_threads:
        for par in args.parallelism:
            for rep in range(1, args.repeats + 1):
                run_num += 1
                if args.saturate:
                    n_copies = max(1, num_cores // g4_t)
                    print(
                        f"[{run_num}/{total_runs}] "
                        f"g4_threads={g4_t} parallelism={par} repeat={rep} "
                        f"copies={n_copies} (saturating {num_cores}-core pool)"
                    )
                else:
                    n_copies = 1
                    print(
                        f"[{run_num}/{total_runs}] "
                        f"g4_threads={g4_t} parallelism={par} repeat={rep}"
                    )

                trace_file = tempfile.mktemp(suffix=".json", prefix="trace_")

                try:
                    config_json = render_jsonnet(
                        args.workflow,
                        args.num_events,
                        g4_t,
                        pythia_threads=par if args.workflow == "pythia8_mt" else None,
                    )
                except subprocess.CalledProcessError as e:
                    print(f"  jsonnet failed: {e.stderr}", file=sys.stderr)
                    continue

                try:
                    if args.saturate:
                        cohort, wall = run_phlex_cohort(
                            config_json, par, g4_t, n_copies, cpu_pool
                        )
                        metrics = aggregate_cohort(cohort)
                        trace_stats = aggregate_cohort_traces(cohort)
                        rc = max((r[0] for r in cohort), default=-1)
                        row = {
                            "workflow": args.workflow,
                            "num_events": args.num_events,
                            "g4_threads": g4_t,
                            "parallelism": par,
                            "copies": n_copies,
                            "repeat": rep,
                            "return_code": rc,
                            "cpu_time_s": metrics.get("cpu_time_s", ""),
                            "real_time_s": metrics.get("real_time_s", ""),
                            "cohort_wall_s": f"{wall:.3f}",
                            "cpu_efficiency_pct": metrics.get("cpu_efficiency_pct", ""),
                            "max_rss_mb": metrics.get("max_rss_mb", ""),
                            **trace_stats,
                        }
                        results.append(row)
                        print(
                            f"  cohort_wall={wall:.2f}s  "
                            f"per-copy real={metrics.get('real_time_s', '?')}s  rc={rc}"
                        )
                        for _, _, tf, od in cohort:
                            if os.path.exists(tf):
                                os.unlink(tf)
                            if os.path.exists(od):
                                shutil.rmtree(od, ignore_errors=True)
                    else:
                        result = run_phlex(config_json, par, trace_file=trace_file)
                        combined_output = result.stdout + result.stderr
                        metrics = parse_phlex_output(combined_output)
                        trace_stats = parse_chrome_trace(trace_file)

                        row = {
                            "workflow": args.workflow,
                            "num_events": args.num_events,
                            "g4_threads": g4_t,
                            "parallelism": par,
                            "copies": 1,
                            "repeat": rep,
                            "return_code": result.returncode,
                            "cpu_time_s": metrics.get("cpu_time_s", ""),
                            "real_time_s": metrics.get("real_time_s", ""),
                            "cohort_wall_s": "",
                            "cpu_efficiency_pct": metrics.get("cpu_efficiency_pct", ""),
                            "max_rss_mb": metrics.get("max_rss_mb", ""),
                            **trace_stats,
                        }
                        results.append(row)
                        rt = metrics.get("real_time_s", "?")
                        eff = metrics.get("cpu_efficiency_pct", "?")
                        print(
                            f"  real={rt}s  efficiency={eff}%  rc={result.returncode}"
                        )

                except subprocess.TimeoutExpired:
                    print("  TIMEOUT", file=sys.stderr)
                    results.append(
                        {
                            "workflow": args.workflow,
                            "num_events": args.num_events,
                            "g4_threads": g4_t,
                            "parallelism": par,
                            "copies": n_copies,
                            "repeat": rep,
                            "return_code": -1,
                        }
                    )
                finally:
                    if os.path.exists(config_json):
                        os.unlink(config_json)
                    if os.path.exists(trace_file):
                        os.unlink(trace_file)
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

    # 3. Mean framework overhead per event vs g4_threads
    for par in par_levels:
        xs, ys = [], []
        for g4_t in g4_levels:
            v = avg(grouped.get((g4_t, par), []), "mean_overhead_ms")
            if v is not None:
                xs.append(g4_t)
                ys.append(v)
        if xs:
            axes[1, 0].plot(xs, ys, "o-", label=f"parallelism={par}")
    axes[1, 0].set_xlabel("G4 threads")
    axes[1, 0].set_ylabel("Mean framework overhead per event (ms)")
    axes[1, 0].set_title("simulate − ProcessOneEvent vs G4 threads")
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
    parser.add_argument(
        "--saturate",
        action="store_true",
        help=(
            "For each thread count T, run floor(num_cores / T) parallel "
            "phlex processes pinned to disjoint CPU slices, so the machine "
            "is fully loaded for every row. Produces per-cohort metrics."
        ),
    )
    args = parser.parse_args()

    # Check dependencies
    if not shutil.which("jsonnet"):
        print("Error: jsonnet not found in PATH", file=sys.stderr)
        sys.exit(1)
    if not shutil.which("phlex"):
        print("Error: phlex not found in PATH", file=sys.stderr)
        sys.exit(1)
    if args.saturate and not shutil.which("taskset"):
        print("Error: --saturate requires taskset in PATH", file=sys.stderr)
        sys.exit(1)

    results = run_sweep(args)
    write_csv(results, args.output)

    if args.plot:
        plot_results(results, args.output)


if __name__ == "__main__":
    main()
