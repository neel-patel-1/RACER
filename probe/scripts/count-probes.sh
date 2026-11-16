#!/usr/bin/env bash
# Counts the probe points in each library of a build: the calls to do_yield
# that the pass put in (one at each loop header it probed).
#   scripts/count-probes.sh build/interval-200
set -euo pipefail
build_dir=${1:?usage: count-probes.sh <build dir>}
llvm_objdump=$(command -v llvm-objdump-17 || command -v llvm-objdump)

echo "library,probe_points"
for library in \
    workloads/libracer_kernels.a \
    workloads/libracer_router_proto.a \
    install/lib/libprotobuf.a \
    install/lib/libprotoc.a \
    ipp-crypto/.build/RELEASE/lib/libippcp.a \
    install/lib/libabsl_strings.a; do
  probe_points=$("$llvm_objdump" -r "$build_dir/$library" | grep -c 'do_yield' || true)
  echo "$(basename "$library"),$probe_points"
done
