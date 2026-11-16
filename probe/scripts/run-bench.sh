#!/usr/bin/env bash
# Runs preempt_bench of each built configuration (build/off and
# build/interval-*) and writes one CSV for each mode:
#   overhead.csv      OVERHEAD_SWEEPS sweeps over the builds (default 10), one
#                     after the other, so that a slow period of the host falls
#                     on all builds; summarize.py takes the median of the sweeps
#   latency.csv       one sweep over the builds; samples-<build>.csv holds
#                     each run of the build
#   signal-floor.csv  one run; the floor does not depend on the probes
#   scripts/run-bench.sh results/2026-11-15 --size 65536 --cpu 2 --accelerator-cpu 4
#   scripts/summarize.py results/2026-11-15
# The arguments after the output directory go to every preempt_bench run.
# The filler and accelerator CPUs must be idle: on a loaded host the OS takes
# the accelerator CPU away and late_completions in latency.csv goes up.
set -euo pipefail
output_dir=${1:?usage: run-bench.sh <output dir> [preempt_bench options]}
shift
overhead_sweeps=${OVERHEAD_SWEEPS:-10}
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

# Runs each bench in the mode once, with one CSV header in all.
sweep() {
  local mode=$1 print_header=$2
  shift 2
  for bench in "${benches[@]}"; do
    header_option=()
    [ "$print_header" = no ] && header_option=(--no-header)
    echo "$mode: $bench" >&2
    "$bench" --mode "$mode" "${header_option[@]}" "$@"
    print_header=no
  done
}

for sweep_number in $(seq 1 "$overhead_sweeps"); do
  print_header=no
  [ "$sweep_number" -eq 1 ] && print_header=yes
  sweep overhead $print_header "$@"
done > "$output_dir/overhead.csv"
print_header=yes
for bench in "${benches[@]}"; do
  build_name=$(basename "$(dirname "$(dirname "$bench")")")
  header_option=()
  [ $print_header = no ] && header_option=(--no-header)
  echo "latency: $bench" >&2
  "$bench" --mode latency "${header_option[@]}" --samples "$output_dir/samples-$build_name.csv" "$@"
  print_header=no
done > "$output_dir/latency.csv"
"${benches[0]}" --mode signal-floor "$@" > "$output_dir/signal-floor.csv"
echo "wrote overhead.csv, latency.csv and signal-floor.csv in $output_dir" >&2
