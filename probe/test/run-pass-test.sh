#!/usr/bin/env bash
# Runs the "; RUN:" lines of one pass test, the way LLVM lit does.
#   %racer-opt   opt with the plugin loaded (so -racer-probe-interval exists)
#   %s           the test file
#   %t           a scratch file path for this test
# usage: run-pass-test.sh TEST.ll LLVM_TOOLS_DIR PLUGIN
set -euo pipefail
test_file=$1
llvm_tools_dir=$2
plugin=$3

export PATH="$llvm_tools_dir:$PATH"
scratch_dir=$(mktemp -d)
trap 'rm -rf "$scratch_dir"' EXIT
scratch_file="$scratch_dir/$(basename "$test_file")"
run_lines=$(sed -n 's/^; RUN: //p' "$test_file")
if [ -z "$run_lines" ]; then
  echo "no RUN lines in $test_file" >&2
  exit 1
fi
while IFS= read -r command; do
  command=${command//%racer-opt/opt -load $plugin -load-pass-plugin $plugin}
  command=${command//%s/$test_file}
  command=${command//%t/$scratch_file}
  echo "RUN: $command"
  bash -o pipefail -c "$command"
done <<< "$run_lines"
