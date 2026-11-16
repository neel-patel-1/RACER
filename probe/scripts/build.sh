#!/usr/bin/env bash
# Builds one configuration into probe/build/<name>.
#   scripts/build.sh 200    probe about every 200 IR instructions
#   scripts/build.sh 0      probe every loop iteration
#   scripts/build.sh off    no probes (baseline)
# Then runs the pass tests.
set -euo pipefail
interval=${1:?usage: build.sh <interval|off>}
probe_dir=$(cd "$(dirname "$0")/.." && pwd)
if [ "$interval" = off ]; then
  build_dir="$probe_dir/build/off"
else
  build_dir="$probe_dir/build/interval-$interval"
fi

# clang picks the newest GCC directory even when it has no libstdc++ (on these
# hosts GCC 14 has none); give it the newest one that does.
newest_gcc_with_libstdcxx=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/ |
  while read -r gcc_dir; do
    [ -e "$gcc_dir/libstdc++.so" ] && echo "${gcc_dir%/}"
  done | sort -V | tail -1)
host_flags="--gcc-install-dir=$newest_gcc_with_libstdcxx"

cmake -S "$probe_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_C_COMPILER=clang-17 -DCMAKE_CXX_COMPILER=clang++-17 \
  -DCMAKE_C_FLAGS="$host_flags" -DCMAKE_CXX_FLAGS="$host_flags" \
  -DRACER_PROBE_INTERVAL="$interval"
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
