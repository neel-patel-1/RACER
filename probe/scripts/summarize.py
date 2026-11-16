#!/usr/bin/env python3
"""Joins the CSVs of run-bench.sh into two Markdown tables.

  scripts/summarize.py results/2026-11-15

Overhead table, one row for each filler and probed build:
  overhead          median runtime of the build / median runtime of off - 1;
                    each median is the median of the sweep medians
  off spread        (largest - smallest) / median of the sweep medians of off:
                    an overhead inside this spread is noise
  probes a run      probe points that one run executes
  time a probe      median runtime / probes a run: the mean time between two
                    probe points
  cost a probe      (median runtime - off median runtime) / probes a run

Latency table, one row for each filler and build, from completion to the
scheduler running again (latency = detect + switch):
  detect            completion to the probe point that saw it (or the end of
                    the filler)
  detect model      signal floor median + time a probe / 2: the wait for the
                    next probe point, if probe points are evenly spaced in time
  switch            the probe point to the scheduler running again
"""
import csv
import math
import statistics
import sys
from pathlib import Path


def read_rows(path):
    with open(path, newline="") as csv_file:
        return list(csv.DictReader(csv_file))


def format_ns(ns):
    # nan: no run of the row has the value (a detect median when no run sees
    # the completion before the filler ends)
    if ns is None or math.isnan(ns):
        return "—"
    if abs(ns) >= 1e6:
        return f"{ns / 1e6:.2f} ms"
    if abs(ns) >= 1e4:
        return f"{ns / 1e3:.1f} µs"
    if abs(ns) >= 1e3:
        return f"{ns / 1e3:.2f} µs"
    if abs(ns) >= 10:
        return f"{ns:.0f} ns"
    return f"{ns:.1f} ns"


def filler_label(row):
    if row["ipp_code_path"] == "native" or not row["filler"].startswith("decrypt"):
        return f"`{row['filler']}`"
    return f"`{row['filler']}`, {row['ipp_code_path']}"


def interval_order(interval):
    return -1 if interval == "off" else int(interval)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: summarize.py <results dir of run-bench.sh>")
    results_dir = Path(sys.argv[1])
    overhead_rows = read_rows(results_dir / "overhead.csv")
    latency_rows = read_rows(results_dir / "latency.csv")
    signal_floor_median_ns = float(read_rows(results_dir / "signal-floor.csv")[0]["median_ns"])

    fillers = list(dict.fromkeys(filler_label(row) for row in overhead_rows))
    intervals = sorted({row["probe_interval"] for row in overhead_rows}, key=interval_order)

    sweep_medians_ns = {}  # (filler, interval) -> the median runtime of each sweep
    probes_a_run = {}  # (filler, interval) -> probe points one run executes
    for row in overhead_rows:
        key = (filler_label(row), row["probe_interval"])
        sweep_medians_ns.setdefault(key, []).append(float(row["median_ns"]))
        probes_a_run[key] = int(row["probe_executions_per_run"])
        if int(row["wrong_outputs"]) != 0:
            print(f"warning: {key} has wrong outputs", file=sys.stderr)
    median_runtime_ns = {key: statistics.median(values) for key, values in sweep_medians_ns.items()}

    def time_a_probe_ns(filler, interval):
        probes = probes_a_run[(filler, interval)]
        return median_runtime_ns[(filler, interval)] / probes if probes else None

    print("| Filler | Off runtime | Off spread | Interval | Overhead | Probes a run "
          "| Time a probe | Cost a probe |")
    print("|---|---|---|---|---|---|---|---|")
    for filler in fillers:
        off_ns = median_runtime_ns[(filler, "off")]
        off_sweeps_ns = sweep_medians_ns[(filler, "off")]
        off_spread = (max(off_sweeps_ns) - min(off_sweeps_ns)) / off_ns
        for interval in intervals:
            if interval == "off":
                continue
            key = (filler, interval)
            probes = probes_a_run[key]
            cost_a_probe = (median_runtime_ns[key] - off_ns) / probes if probes else None
            print(f"| {filler} | {format_ns(off_ns)} | {off_spread:.1%} | {interval} "
                  f"| {median_runtime_ns[key] / off_ns - 1:+.1%} | {probes:,} "
                  f"| {format_ns(time_a_probe_ns(filler, interval))} "
                  f"| {format_ns(cost_a_probe)} |")

    print()
    print(f"Signal floor median: {format_ns(signal_floor_median_ns)}")
    print()
    print("| Filler | Interval | Yields | Detect median / p99 | Detect model "
          "| Switch median / p99 | Latency median / p99 |")
    print("|---|---|---|---|---|---|---|")
    latency_by_key = {(filler_label(row), row["probe_interval"]): row for row in latency_rows}
    for filler in fillers:
        for interval in intervals:
            row = latency_by_key.get((filler, interval))
            if row is None:
                continue
            time_a_probe = time_a_probe_ns(filler, interval)
            detect_model_ns = (signal_floor_median_ns + time_a_probe / 2
                               if time_a_probe is not None else None)
            print(f"| {filler} | {interval} | {row['yielded']} / {row['runs']} "
                  f"| {format_ns(float(row['detect_median_ns']))} / "
                  f"{format_ns(float(row['detect_p99_ns']))} | {format_ns(detect_model_ns)} "
                  f"| {format_ns(float(row['switch_median_ns']))} / "
                  f"{format_ns(float(row['switch_p99_ns']))} "
                  f"| {format_ns(float(row['latency_median_ns']))} / "
                  f"{format_ns(float(row['latency_p99_ns']))} |")


if __name__ == "__main__":
    main()
