#!/usr/bin/env python3
"""Joins the CSVs of run-dsa-bench.sh into two Markdown tables for each DSA
work queue (WQ).

  scripts/summarize-dsa.py results/dsa

Offload table, one row for each dsa-floor run:
  submit            the portal write (MOVDIR64B to a dedicated WQ, ENQCMD
                    until the WQ takes it to a shared WQ), or the write()
                    system call when the driver does not map the portal
  offload           start of the submit to the observer seeing the status byte
                    of the completion record: the device reads the descriptor,
                    moves the bytes and writes the record; no kernel code
  interrupts        idxd interrupts an offload, from /proc/interrupts: 0, as
                    the descriptor asks for no completion interrupt
  done in submit    runs where the byte was set when the submit returned
  floor             the filler CPU seeing the byte - the observer seeing it
  switches          involuntary context switches of the filler and of the
                    observer thread during the measured runs: other work on
                    their CPUs

Latency table, one row for each filler and build. The times are from the
observer seeing the status byte. The device writes the byte, so the observer
sees it about when the filler CPU can see it (the floor), and there is no
observer lag to add:
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
    if int(row["completed_during_submit"]) > 0.01 * int(row["runs"]):
        print(f"warning: {label} has completed_during_submit = "
              f"{row['completed_during_submit']} of {row['runs']}", file=sys.stderr)


def format_switches(row):
    return f"{row['filler_involuntary_switches']} / {row['observer_involuntary_switches']}"


def print_work_queue_tables(work_queue, floor_rows, latency_rows):
    first_floor_row = floor_rows[0]
    print(f"Filler CPU {first_floor_row['cpu']}, observer CPU {first_floor_row['observer_cpu']}")
    print()
    print("| Offload | Device idle before | Submit median | Offload median | Offload p10 / p90 / p99 "
          "| Interrupts an offload | Done in submit | Floor median (p10 / p90) "
          "| Switches filler / observer |")
    print("|---|---|---|---|---|---|---|---|---|")
    for row in floor_rows:
        warn_on_bad_runs(row, f"dsa-floor {work_queue} {row['offload_bytes']} B, idle {row['idle_us']} us")
        idle_ns = float(row["idle_us"]) * 1000
        print(f"| {format_bytes(int(row['offload_bytes']))} | {format_ns(idle_ns) if idle_ns else '0'} "
              f"| {format_ns(float(row['submit_median_ns']))} "
              f"| {format_ns(float(row['offload_median_ns']))} "
              f"| {format_ns(float(row['offload_p10_ns']))} / "
              f"{format_ns(float(row['offload_p90_ns']))} / "
              f"{format_ns(float(row['offload_p99_ns']))} "
              f"| {int(row['idxd_interrupts']) / int(row['runs']):.2f} "
              f"| {row['completed_during_submit']} / {row['runs']} "
              f"| {format_ns(float(row['floor_median_ns']))} "
              f"({format_ns(float(row['floor_p10_ns']))} / "
              f"{format_ns(float(row['floor_p90_ns']))}) "
              f"| {format_switches(row)} |")

    print()
    print("| Filler | Interval | Filler runtime | Yields | Offload median "
          "| Detect median / p99 | Switch median / p99 | Latency median / p99 "
          "| Switches filler / observer |")
    print("|---|---|---|---|---|---|---|---|---|")
    fillers = list(dict.fromkeys(row["filler"] for row in latency_rows))
    latency_by_key = {(row["filler"], row["probe_interval"]): row for row in latency_rows}
    intervals = sorted({row["probe_interval"] for row in latency_rows}, key=interval_order)
    for filler in fillers:
        for interval in intervals:
            row = latency_by_key.get((filler, interval))
            if row is None:
                continue
            warn_on_bad_runs(row, f"dsa-latency {work_queue} {filler} {interval}")
            print(f"| `{filler}` | {interval} | {format_ns(float(row['filler_median_ns']))} "
                  f"| {row['yielded']} / {row['runs']} "
                  f"| {format_ns(float(row['offload_median_ns']))} "
                  f"| {format_ns(float(row['detect_median_ns']))} / "
                  f"{format_ns(float(row['detect_p99_ns']))} "
                  f"| {format_ns(float(row['switch_median_ns']))} / "
                  f"{format_ns(float(row['switch_p99_ns']))} "
                  f"| {format_ns(float(row['latency_median_ns']))} / "
                  f"{format_ns(float(row['latency_p99_ns']))} "
                  f"| {format_switches(row)} |")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: summarize-dsa.py <results dir of run-dsa-bench.sh>")
    results_dir = Path(sys.argv[1])
    for work_queue_row in read_rows(results_dir / "work-queues.csv"):
        work_queue = work_queue_row["work_queue"]
        print(f"## {work_queue} ({work_queue_row['mode']} WQ of {work_queue_row['device']}, "
              f"NUMA node {work_queue_row['device_numa_node']}, "
              f"block_on_fault {work_queue_row['block_on_fault']})")
        print()
        print_work_queue_tables(work_queue,
                                read_rows(results_dir / f"dsa-floor-{work_queue}.csv"),
                                read_rows(results_dir / f"dsa-latency-{work_queue}.csv"))
        print()


if __name__ == "__main__":
    main()
