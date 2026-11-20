#!/usr/bin/env bash
# Cuts the switch of a yield into parts (--switch-parts) in a device latency
# mode, with each jump of do_yield (--yield-jump), and measures the switch
# floor of each jump on the filler CPU. Each sweep writes into sweep-<n>/:
#   latency-<jump>-plain.csv  the latency mode with no part stamps
#   latency-<jump>-parts.csv  the latency mode with the part stamps
#   parts-<jump>.csv          the part medians of each filler
#   switch-floor.csv          switch-floor of each jump, with and without stamps
#   parts-floor-<jump>.csv    the part medians of the yield loop
#   scripts/run-switch-parts.sh results/switch-parts/qat qat-latency 38 110 --size 1048576
#   scripts/run-switch-parts.sh results/switch-parts/dsa dsa-latency 2 42 --size 1048576
#   scripts/summarize-switch-parts.py results/switch-parts/qat
# PREEMPT_BENCH selects the binary (default build/interval-200), FILLERS the
# fillers, SWEEPS the number of sweeps (default 3). WARM_UP_SECONDS (default 0)
# keeps the filler CPU busy for that time before each run: the ondemand
# governor of emerald-03 holds a busy core at 0.8 GHz for up to 8 s or more
# before it raises the clock, and the clock stays up from one run to the next.
# The arguments after the observer CPU go to every latency run.
set -euo pipefail
usage="usage: run-switch-parts.sh <output dir> qat-latency|dsa-latency|latency <filler cpu> <observer cpu> [preempt_bench options]"
output_dir=${1:?$usage}
latency_mode=${2:?$usage}
filler_cpu=${3:?$usage}
observer_cpu=${4:?$usage}
shift 4
probe_dir=$(cd "$(dirname "$0")/.." && pwd)
bench=${PREEMPT_BENCH:-$probe_dir/build/interval-200/workloads/preempt_bench}
fillers=${FILLERS:-hash dotproduct histogram matmul gather scatter-update sequential-writer}
sweeps=${SWEEPS:-3}
warm_up_seconds=${WARM_UP_SECONDS:-0}
yield_jumps=(boost own-copy no-fpu-control-load)

if [ ! -x "$bench" ]; then
  echo "no preempt_bench at $bench" >&2
  exit 1
fi
case "$latency_mode" in
  latency) cpu_options=(--cpu "$filler_cpu" --accelerator-cpu "$observer_cpu") ;;
  qat-latency | dsa-latency) cpu_options=(--cpu "$filler_cpu" --observer-cpu "$observer_cpu") ;;
  *) echo "$usage" >&2; exit 1 ;;
esac
filler_options=()
for filler in $fillers; do
  filler_options+=(--filler "$filler")
done

warm_up_filler_cpu() {
  if [ "$warm_up_seconds" -gt 0 ]; then
    taskset -c "$filler_cpu" timeout "$warm_up_seconds" bash -c 'while :; do :; done' || true
  fi
}

for sweep in $(seq 1 "$sweeps"); do
  sweep_dir=$output_dir/sweep-$sweep
  mkdir -p "$sweep_dir"
  for yield_jump in "${yield_jumps[@]}"; do
    echo "sweep $sweep: $latency_mode, $yield_jump" >&2
    warm_up_filler_cpu
    "$bench" --mode "$latency_mode" "${cpu_options[@]}" "${filler_options[@]}" "$@" \
      --yield-jump "$yield_jump" > "$sweep_dir/latency-$yield_jump-plain.csv"
    "$bench" --mode "$latency_mode" "${cpu_options[@]}" "${filler_options[@]}" "$@" \
      --yield-jump "$yield_jump" --switch-parts "$sweep_dir/parts-$yield_jump.csv" \
      > "$sweep_dir/latency-$yield_jump-parts.csv"
  done
  echo "sweep $sweep: switch-floor" >&2
  warm_up_filler_cpu
  {
    print_header=yes
    for yield_jump in "${yield_jumps[@]}"; do
      header_option=()
      [ $print_header = no ] && header_option=(--no-header)
      "$bench" --mode switch-floor "${header_option[@]}" --cpu "$filler_cpu" \
        --yield-jump "$yield_jump"
      "$bench" --mode switch-floor --no-header --cpu "$filler_cpu" \
        --yield-jump "$yield_jump" --switch-parts "$sweep_dir/parts-floor-$yield_jump.csv"
      print_header=no
    done
  } > "$sweep_dir/switch-floor.csv"
done
