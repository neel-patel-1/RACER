#!/usr/bin/env bash
# Runs the DSA modes of preempt_bench on each user DSA work queue (WQ) that
# this user can open, and writes for each WQ:
#   dsa-floor-<wq>.csv    the offload time and the floor at 16 B, 4 KiB and
#                         64 KiB, then at 4 KiB after 0.1, 1, 10 and 100 ms of
#                         device idle
#   dsa-latency-<wq>.csv  dsa-latency of each built configuration (build/off and
#                         build/interval-*); dsa-samples-<wq>-<build>.csv holds
#                         each run
# and work-queues.csv: the mode (dedicated or shared), block_on_fault and the
# NUMA node of the device of each WQ.
#   scripts/run-dsa-bench.sh results/dsa 2 42 --size 1048576
#   scripts/summarize-dsa.py results/dsa
# Put the filler CPU on the NUMA node of the DSA device, and the observer on
# the SMT sibling of the filler CPU: the two see the completion record at the
# same time. The device writes the record, so there is no observer lag to
# measure (run-qat-bench.sh measures the lag from the CPU of the QAT
# interrupt). The arguments after the observer CPU go to every dsa run.
set -euo pipefail
usage="usage: run-dsa-bench.sh <output dir> <filler cpu> <observer cpu> [preempt_bench options]"
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

work_queue_paths=()
for work_queue_path in /dev/dsa/wq*; do
  [ -r "$work_queue_path" ] && [ -w "$work_queue_path" ] && work_queue_paths+=("$work_queue_path")
done
if [ ${#work_queue_paths[@]} -eq 0 ]; then
  echo "no DSA WQ under /dev/dsa that this user can open; an administrator makes one" >&2
  exit 1
fi

echo "work_queue,mode,block_on_fault,device,device_numa_node" > "$output_dir/work-queues.csv"
for work_queue_path in "${work_queue_paths[@]}"; do
  work_queue=$(basename "$work_queue_path")
  sysfs_dir=/sys/bus/dsa/devices/$work_queue
  device=dsa${work_queue#wq}
  device=${device%.*}
  echo "$work_queue,$(cat "$sysfs_dir/mode"),$(cat "$sysfs_dir/block_on_fault"),$device,$(cat "/sys/bus/dsa/devices/$device/numa_node")" \
    >> "$output_dir/work-queues.csv"
done

for work_queue_path in "${work_queue_paths[@]}"; do
  work_queue=$(basename "$work_queue_path")
  common_options=(--cpu "$filler_cpu" --observer-cpu "$observer_cpu" --work-queue "$work_queue_path")

  echo "dsa-floor $work_queue: ${benches[0]}" >&2
  {
    print_header=yes
    for offload_bytes in 16 4096 65536; do
      header_option=()
      [ $print_header = no ] && header_option=(--no-header)
      "${benches[0]}" --mode dsa-floor "${header_option[@]}" "${common_options[@]}" "$@" \
        --offload-bytes "$offload_bytes"
      print_header=no
    done
    # Each run is at least --idle-us long, so fewer runs.
    for idle_us in 100 1000 10000 100000; do
      "${benches[0]}" --mode dsa-floor --no-header "${common_options[@]}" "$@" \
        --offload-bytes 4096 --idle-us "$idle_us" --runs 200
    done
  } > "$output_dir/dsa-floor-$work_queue.csv"

  print_header=yes
  for bench in "${benches[@]}"; do
    build_name=$(basename "$(dirname "$(dirname "$bench")")")
    header_option=()
    [ $print_header = no ] && header_option=(--no-header)
    echo "dsa-latency $work_queue: $bench" >&2
    "$bench" --mode dsa-latency "${header_option[@]}" "${common_options[@]}" "$@" \
      --samples "$output_dir/dsa-samples-$work_queue-$build_name.csv"
    print_header=no
  done > "$output_dir/dsa-latency-$work_queue.csv"
done
echo "wrote work-queues.csv and the dsa-floor and dsa-latency CSVs of each WQ in $output_dir" >&2
