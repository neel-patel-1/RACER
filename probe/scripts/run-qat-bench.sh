#!/usr/bin/env bash
# Runs the QAT modes of preempt_bench and writes three CSVs:
#   qat-floor.csv     the offload time and the floor at 16 B, 4 KiB and 64 KiB,
#                     then at 4 KiB after 0.1, 1, 10 and 100 ms of device idle
#   observer-lag.csv  signal-floor from the CPU of the QAT interrupt (the
#                     busiest_interrupt_cpu of qat-floor.csv) to the observer
#                     CPU: the time from the kernel write of the completion
#                     byte to the observer seeing it
#   qat-latency.csv   qat-latency of each built configuration (build/off and
#                     build/interval-*); qat-samples-<build>.csv holds each run
#   scripts/run-qat-bench.sh results/qat 38 110 --size 1048576
#   scripts/summarize-qat.py results/qat
# The filler CPU picks the QAT instance (the instance on its NUMA node), and so
# the interrupt CPU. An observer on the SMT sibling of the filler CPU sees the
# completion byte at the same time as the filler CPU. The arguments after the
# observer CPU go to every qat run.
set -euo pipefail
usage="usage: run-qat-bench.sh <output dir> <filler cpu> <observer cpu> [preempt_bench options]"
output_dir=${1:?$usage}
filler_cpu=${2:?$usage}
observer_cpu=${3:?$usage}
shift 3
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
cpu_options=(--cpu "$filler_cpu" --observer-cpu "$observer_cpu")

echo "qat-floor: ${benches[0]}" >&2
{
  print_header=yes
  for offload_bytes in 16 4096 65536; do
    header_option=()
    [ $print_header = no ] && header_option=(--no-header)
    "${benches[0]}" --mode qat-floor "${header_option[@]}" "${cpu_options[@]}" "$@" \
      --offload-bytes "$offload_bytes"
    print_header=no
  done
  # Each run is at least --idle-us long, so fewer runs.
  for idle_us in 100 1000 10000 100000; do
    "${benches[0]}" --mode qat-floor --no-header "${cpu_options[@]}" "$@" \
      --offload-bytes 4096 --idle-us "$idle_us" --runs 200
  done
} > "$output_dir/qat-floor.csv"

interrupt_cpu=$(python3 -c '
import csv, sys
print(next(csv.DictReader(open(sys.argv[1])))["busiest_interrupt_cpu"])' "$output_dir/qat-floor.csv")
echo "signal-floor: QAT interrupt CPU $interrupt_cpu to observer CPU $observer_cpu" >&2
"${benches[0]}" --mode signal-floor --cpu "$observer_cpu" --accelerator-cpu "$interrupt_cpu" \
  > "$output_dir/observer-lag.csv"

print_header=yes
for bench in "${benches[@]}"; do
  build_name=$(basename "$(dirname "$(dirname "$bench")")")
  header_option=()
  [ $print_header = no ] && header_option=(--no-header)
  echo "qat-latency: $bench" >&2
  "$bench" --mode qat-latency "${header_option[@]}" "${cpu_options[@]}" "$@" \
    --samples "$output_dir/qat-samples-$build_name.csv"
  print_header=no
done > "$output_dir/qat-latency.csv"
echo "wrote qat-floor.csv, observer-lag.csv and qat-latency.csv in $output_dir" >&2
