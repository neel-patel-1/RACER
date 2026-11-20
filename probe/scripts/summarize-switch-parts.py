#!/usr/bin/env python3
"""Joins the CSVs of run-switch-parts.sh into three Markdown tables.

  scripts/summarize-switch-parts.py results/switch-parts/qat
  scripts/summarize-switch-parts.py results/switch-parts/qat --core-ghz 2.18

Each value is the median over the sweeps of the median of each sweep; ns,
and core cycles at the median core_ghz of the switch-floor rows. --core-ghz
sets the clock instead: a binary from before the fastest-chain clock measure
gives a low core_ghz when another task shares the filler CPU.

The switch is from the filler stop (the entry stamp of do_yield) to the
scheduler stamp after the jump. The parts, from the stamps of --switch-parts:
  stamp         one read_tsc (lfence; rdtsc; lfence). Each switch has one: the
                filler stop is the end of a stamp, the scheduler stamp ends
                after its rdtsc.
  checks        the checks of do_yield and the store of the filler stop time
  store drain   an mfence: the wait for the stores of the filler to leave the
                store buffer. The mfence is not in a switch with no part
                stamps, and it costs time with an empty store buffer, so the
                tables show the store drain minus the store drain of the yield
                loop (a fiber that only yields: few stores). This is the most
                time that the stores of the filler can add: without the
                mfence, the jump waits only for free store buffer entries.
  context load  a load of the first and the last byte of the scheduler
                context: a cache miss when the filler evicted the context
  jump          the call to jump_fcontext: it saves the filler registers,
                loads the scheduler registers (ldmxcsr and fldcw too), and
                jumps into the scheduler with an indirect jump
The sum of the stamp and the four parts (the store drain minus that of the
yield loop) is the model of the switch with no part stamps.

The stamp is a fixed number of core cycles (about 58 with the SMT sibling
idle, 63-69 with the sibling busy), so a stamp that is different by more than
10 % from one filler to the next shows that the core clock changed between or
in the runs. Then the script prints a note: the cycles at one clock are not
exact.
"""
import argparse
import csv
import statistics
import sys
from pathlib import Path

YIELD_JUMPS = ("boost", "own-copy", "no-fpu-control-load")
STAMP_SPREAD_LIMIT = 0.10


def read_rows(path):
    with open(path, newline="") as csv_file:
        return list(csv.DictReader(csv_file))


def median_over_sweeps(rows, column):
    return statistics.median(float(row[column]) for row in rows)


def ns_and_cycles(ns, core_ghz):
    return f"{ns:.1f} ({ns * core_ghz:.0f})"


def sweep_dirs(results_dir):
    dirs = sorted(results_dir.glob("sweep-*"))
    if not dirs:
        sys.exit(f"no sweep-* directory in {results_dir}")
    return dirs


def rows_by_filler(sweeps, file_name):
    """The rows of file_name of each sweep, by filler."""
    by_filler = {}
    for sweep in sweeps:
        for row in read_rows(sweep / file_name):
            by_filler.setdefault(row["filler"], []).append(row)
    return by_filler


def main():
    parser = argparse.ArgumentParser(description="Joins the CSVs of run-switch-parts.sh.")
    parser.add_argument("results_dir", type=Path, help="the output dir of run-switch-parts.sh")
    parser.add_argument("--core-ghz", type=float,
                        help="the core clock for the cycles (default: from the switch-floor rows)")
    arguments = parser.parse_args()
    sweeps = sweep_dirs(arguments.results_dir)

    floor_rows = [row for sweep in sweeps for row in read_rows(sweep / "switch-floor.csv")]
    floor_parts = {jump: [row for sweep in sweeps
                          for row in read_rows(sweep / f"parts-floor-{jump}.csv")]
                   for jump in YIELD_JUMPS}
    core_ghz = arguments.core_ghz or median_over_sweeps(floor_rows, "core_ghz")
    print(f"Filler CPU {floor_rows[0]['cpu']}, {len(sweeps)} sweeps, core {core_ghz:.2f} GHz; "
          "ns (core cycles)")
    print()

    print("Switch floor (a fiber that only yields):")
    print()
    print("| Jump | Switch, no part stamps | Stamp | Checks | Store drain (mfence) "
          "| Context load | Jump | Model |")
    print("|---|---|---|---|---|---|---|---|")
    floor_store_drain_ns = {}
    for jump in YIELD_JUMPS:
        plain = [row for row in floor_rows if row["yield_jump"] == jump and row["switch_parts"] == "0"]
        parts = floor_parts[jump]
        switch_ns = median_over_sweeps(plain, "switch_median_ns")
        part_ns = {column: median_over_sweeps(parts, f"{column}_median_ns")
                   for column in ("stamp", "checks", "store_drain", "context_load", "jump")}
        floor_store_drain_ns[jump] = part_ns["store_drain"]
        model_ns = part_ns["stamp"] + part_ns["checks"] + part_ns["context_load"] + part_ns["jump"]
        print(f"| {jump} | {ns_and_cycles(switch_ns, core_ghz)} "
              + "".join(f"| {ns_and_cycles(part_ns[column], core_ghz)} "
                        for column in ("stamp", "checks", "store_drain", "context_load", "jump"))
              + f"| {ns_and_cycles(model_ns, core_ghz)} |")

    print()
    print("Switch of each filler, jump boost (store drain minus the yield-loop store drain):")
    print()
    print("| Filler | Switch, no part stamps | Stamp | Checks | Store drain | Context load (p90) "
          "| Jump (p90) | Model |")
    print("|---|---|---|---|---|---|---|---|")
    plain_by_filler = rows_by_filler(sweeps, "latency-boost-plain.csv")
    parts_by_filler = rows_by_filler(sweeps, "parts-boost.csv")
    stamp_ns_of_fillers = []
    for filler, parts in parts_by_filler.items():
        switch_ns = median_over_sweeps(plain_by_filler[filler], "switch_median_ns")
        stamp_ns = median_over_sweeps(parts, "stamp_median_ns")
        stamp_ns_of_fillers.append(stamp_ns)
        checks_ns = median_over_sweeps(parts, "checks_median_ns")
        store_drain_ns = median_over_sweeps(parts, "store_drain_median_ns") - floor_store_drain_ns["boost"]
        context_load_ns = median_over_sweeps(parts, "context_load_median_ns")
        context_load_p90_ns = median_over_sweeps(parts, "context_load_p90_ns")
        jump_ns = median_over_sweeps(parts, "jump_median_ns")
        jump_p90_ns = median_over_sweeps(parts, "jump_p90_ns")
        model_ns = stamp_ns + checks_ns + store_drain_ns + context_load_ns + jump_ns
        print(f"| `{filler}` | {ns_and_cycles(switch_ns, core_ghz)} "
              f"| {ns_and_cycles(stamp_ns, core_ghz)} | {ns_and_cycles(checks_ns, core_ghz)} "
              f"| {ns_and_cycles(store_drain_ns, core_ghz)} "
              f"| {ns_and_cycles(context_load_ns, core_ghz)} ({context_load_p90_ns:.0f}) "
              f"| {ns_and_cycles(jump_ns, core_ghz)} ({jump_p90_ns:.0f}) "
              f"| {ns_and_cycles(model_ns, core_ghz)} |")
    stamp_spread = (max(stamp_ns_of_fillers) - min(stamp_ns_of_fillers)) / statistics.median(stamp_ns_of_fillers)
    if stamp_spread > STAMP_SPREAD_LIMIT:
        print()
        print(f"Note: the stamp is {min(stamp_ns_of_fillers):.1f}-{max(stamp_ns_of_fillers):.1f} ns over "
              f"the fillers ({stamp_spread:.0%} of its median). The stamp is a fixed number of core "
              "cycles, so the core clock changed between or in the runs, and the cycles at one clock "
              "are not exact.")

    print()
    print("Jump of each filler by yield jump, with part stamps (the same do_yield code for each "
          "jump), and the switch with no part stamps (boost takes the do_yield code with no "
          "experiment call):")
    print()
    print("| Filler | " + " | ".join(f"Jump {jump}" for jump in YIELD_JUMPS) + " | "
          + " | ".join(f"Switch {jump}" for jump in YIELD_JUMPS) + " |")
    print("|---|" + "---|" * (2 * len(YIELD_JUMPS)))
    jump_parts = {jump: rows_by_filler(sweeps, f"parts-{jump}.csv") for jump in YIELD_JUMPS}
    jump_plain = {jump: rows_by_filler(sweeps, f"latency-{jump}-plain.csv") for jump in YIELD_JUMPS}
    floor_jump_cells = [ns_and_cycles(median_over_sweeps(floor_parts[jump], "jump_median_ns"), core_ghz)
                        for jump in YIELD_JUMPS]
    floor_switch_cells = [
        ns_and_cycles(median_over_sweeps(
            [row for row in floor_rows if row["yield_jump"] == jump and row["switch_parts"] == "0"],
            "switch_median_ns"), core_ghz)
        for jump in YIELD_JUMPS]
    print("| yield loop | " + " | ".join(floor_jump_cells + floor_switch_cells) + " |")
    for filler in parts_by_filler:
        jump_cells = [ns_and_cycles(median_over_sweeps(jump_parts[jump][filler], "jump_median_ns"),
                                    core_ghz) for jump in YIELD_JUMPS]
        switch_cells = [ns_and_cycles(median_over_sweeps(jump_plain[jump][filler], "switch_median_ns"),
                                      core_ghz) for jump in YIELD_JUMPS]
        print(f"| `{filler}` | " + " | ".join(jump_cells + switch_cells) + " |")


if __name__ == "__main__":
    main()
