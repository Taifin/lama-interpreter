#!/usr/bin/env bash
set -euo pipefail

# Usage: ./performance_sort.sh
# Requirements:
# - lamac available in PATH
# - CMake and Make available
# - Project sources for lama_interpreter in current directory

PERF_DIR="performance"
SORT_LAMA="${PERF_DIR}/Sort.lama"
SORT_BC="${PERF_DIR}/Sort.bc"
SORT_SOL="${PERF_DIR}/Sort.sol"
SORT_SOUT="${PERF_DIR}/Sort.sout"
SORT_IOUT="${PERF_DIR}/Sort.iout"
SORT_BOUT="${PERF_DIR}/Sort.bout"
ERR_FILE="${PERF_DIR}/Sort.err"

BUILD_DIR="build"
mkdir -p "$BUILD_DIR"
pushd "$BUILD_DIR" >/dev/null
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
[[ -x "./lama_interpreter" ]] || { echo "Error: build did not produce lama_interpreter"; popd >/dev/null; exit 1; }
LAMA_INTERPRETER="$(pwd)/lama_interpreter"
popd >/dev/null

pushd "$PERF_DIR" >/dev/null
lamac -b "Sort.lama"
popd >/dev/null

time_cmd() {
  local out="$1"; shift
  local start end
  start=$(date +%s.%N)
  if ! "$@" > "$out" 2>"$ERR_FILE"; then
    echo "ERROR: command failed: $*"
  fi
  end=$(date +%s.%N)
  if command -v bc >/dev/null 2>&1; then
    echo "$(printf "%s-%s\n" "$end" "$start" | bc -l)"
  else
    awk -v s="$start" -v e="$end" 'BEGIN{print e-s}'
  fi
}

touch "performance/sort.input"

echo "Run lamac -i"
t_i=$(time_cmd "$SORT_SOL" lamac -i "$SORT_LAMA" < "performance/sort.input")

echo "Run lamac -s"
t_s=$(time_cmd "$SORT_SOUT" lamac -s "$SORT_LAMA" < "performance/sort.input")
if ! diff -q "$SORT_SOL" "$SORT_SOUT" >/dev/null 2>&1; then
  echo "ERROR: lamac -s output differs from lamac -i"
fi

echo "Run bytecode interpreter"
t_b=$(time_cmd "$SORT_BOUT" "$LAMA_INTERPRETER" "$SORT_BC")
if ! diff -q "$SORT_SOL" "$SORT_BOUT" >/dev/null 2>&1; then
  echo "ERROR: lama_interpreter output differs from lamac -i"
fi

echo "Timings (seconds):"
echo "lamac -i: $t_i"
echo "lamac -s: $t_s"
echo "lama_interpreter (bytecode): $t_b"
