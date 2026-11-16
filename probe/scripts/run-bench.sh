#!/usr/bin/env bash
# Runs preempt_bench of each built configuration (build/interval-* and
# build/off) in overhead and latency mode, and writes one CSV for each mode.
#   scripts/run-bench.sh results/2026-11-15 --size 65536 --cpu 2 --accelerator-cpu 4
# The arguments after the output directory go to every preempt_bench run.
# The filler and accelerator CPUs must be idle: on a loaded host the OS takes
# the accelerator CPU away and late_completions in latency.csv goes up.
set -euo pipefail
output_dir=${1:?usage: run-bench.sh <output dir> [preempt_bench options]}
shift
probe_dir=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$output_dir"

benches=()
for build_dir in "$probe_dir"/build/off "$probe_dir"/build/interval-*; do
  [ -x "$build_dir/workloads/preempt_bench" ] && benches+=("$build_dir/workloads/preempt_bench")
done
if [ ${#benches[@]} -eq 0 ]; then
  echo "no built preempt_bench under $probe_dir/build; run scripts/build.sh first" >&2
  exit 1
fi

for mode in overhead latency; do
  print_header=yes
  for bench in "${benches[@]}"; do
    header_option=()
    [ $print_header = no ] && header_option=(--no-header)
    echo "$mode: $bench" >&2
    "$bench" --mode $mode "${header_option[@]}" "$@"
    print_header=no
  done > "$output_dir/$mode.csv"
done
echo "wrote $output_dir/overhead.csv and $output_dir/latency.csv" >&2
