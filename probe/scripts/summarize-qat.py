#!/usr/bin/env python3
"""Joins the CSVs of run-qat-bench.sh into two Markdown tables.

  scripts/summarize-qat.py results/qat

Offload table, one row for each qat-floor run:
  submit            time of the scheduler in sendmsg and io_submit
  offload           start of the submit to the observer seeing the completion
                    byte: the copy into the kernel, the device, its interrupt
                    and the kernel completion
  interrupts        QAT interrupts an offload, from /proc/interrupts: 1 is the
                    completion; more are device power-state interrupts (or QAT
                    work of other users)
  done in submit    runs where the byte was set when io_submit returned: the
                    device is asynchronous when this is about 0
  floor             the filler CPU seeing the byte - the observer seeing it

Latency table, one row for each filler and build. The device gives no time
stamp, so the times are from the observer seeing the completion byte. The
observer lag (observer-lag.csv: signal-floor from the interrupt CPU to the
observer CPU) is the time from the kernel write of the byte to the observer
seeing it; "from write" columns add its median:
  detect            observer seeing the byte to the probe point that saw it
                    (or the end of the filler)
  switch            the probe point to the scheduler running again
  latency           detect + switch
"""
import csv
import math
import sys
from pathlib import Path


def read_rows(path):
    with open(path, newline="") as csv_file:
        return list(csv.DictReader(csv_file))


def format_ns(ns):
    # nan: no run of the row has the value
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


def format_bytes(count):
    if count >= 1024 and count % 1024 == 0:
        return f"{count // 1024} KiB"
    return f"{count} B"


def interval_order(interval):
    return -1 if interval == "off" else int(interval)


def warn_on_bad_runs(row, label):
    for column in ("wrong_outputs", "failed_offloads"):
        if int(row.get(column, 0)) != 0:
            print(f"warning: {label} has {column} = {row[column]}", file=sys.stderr)
    # A few runs with a slow submit are not a problem.
    if int(row["completed_during_submit"]) > 0.01 * int(row["runs"]):
        print(f"warning: {label} has completed_during_submit = "
              f"{row['completed_during_submit']} of {row['runs']}", file=sys.stderr)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: summarize-qat.py <results dir of run-qat-bench.sh>")
    results_dir = Path(sys.argv[1])
    floor_rows = read_rows(results_dir / "qat-floor.csv")
    latency_rows = read_rows(results_dir / "qat-latency.csv")
    observer_lag_ns = float(read_rows(results_dir / "observer-lag.csv")[0]["median_ns"])

    first_floor_row = floor_rows[0]
    print(f"Filler CPU {first_floor_row['cpu']}, observer CPU {first_floor_row['observer_cpu']}, "
          f"QAT interrupt CPU {first_floor_row['busiest_interrupt_cpu']}; "
          f"observer lag median {format_ns(observer_lag_ns)}")
    print()
    print("| Offload | Device idle before | Submit median | Offload median | Offload p10 / p90 / p99 "
          "| Interrupts an offload | Done in submit | Floor median (p10 / p90) |")
    print("|---|---|---|---|---|---|---|---|")
    for row in floor_rows:
        warn_on_bad_runs(row, f"qat-floor {row['offload_bytes']} B, idle {row['idle_us']} us")
        idle_ns = float(row["idle_us"]) * 1000
        print(f"| {format_bytes(int(row['offload_bytes']))} | {format_ns(idle_ns) if idle_ns else '0'} "
              f"| {format_ns(float(row['submit_median_ns']))} "
              f"| {format_ns(float(row['offload_median_ns']))} "
              f"| {format_ns(float(row['offload_p10_ns']))} / "
              f"{format_ns(float(row['offload_p90_ns']))} / "
              f"{format_ns(float(row['offload_p99_ns']))} "
              f"| {int(row['qat_interrupts']) / int(row['runs']):.2f} "
              f"| {row['completed_during_submit']} / {row['runs']} "
              f"| {format_ns(float(row['floor_median_ns']))} "
              f"({format_ns(float(row['floor_p10_ns']))} / "
              f"{format_ns(float(row['floor_p90_ns']))}) |")

    print()
    print("| Filler | Interval | Filler runtime | Yields | Offload median "
          "| Detect median / p99 | Switch median / p99 | Latency from write median / p99 |")
    print("|---|---|---|---|---|---|---|---|")
    fillers = list(dict.fromkeys(row["filler"] for row in latency_rows))
    latency_by_key = {(row["filler"], row["probe_interval"]): row for row in latency_rows}
    intervals = sorted({row["probe_interval"] for row in latency_rows}, key=interval_order)
    for filler in fillers:
        for interval in intervals:
            row = latency_by_key.get((filler, interval))
            if row is None:
                continue
            warn_on_bad_runs(row, f"qat-latency {filler} {interval}")
            print(f"| `{filler}` | {interval} | {format_ns(float(row['filler_median_ns']))} "
                  f"| {row['yielded']} / {row['runs']} "
                  f"| {format_ns(float(row['offload_median_ns']))} "
                  f"| {format_ns(float(row['detect_median_ns']))} / "
                  f"{format_ns(float(row['detect_p99_ns']))} "
                  f"| {format_ns(float(row['switch_median_ns']))} / "
                  f"{format_ns(float(row['switch_p99_ns']))} "
                  f"| {format_ns(float(row['latency_median_ns']) + observer_lag_ns)} / "
                  f"{format_ns(float(row['latency_p99_ns']) + observer_lag_ns)} |")


if __name__ == "__main__":
    main()
