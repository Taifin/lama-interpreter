#!/usr/bin/env bash
set -euo pipefail

# Usage: ./run_tests.sh
# Requirements:
# - lamac available in PATH
# - CMake and Make available
# - Project sources for lama_interpreter in current directory

# I shamelessly declare that this script was produced with the help of chatgpt :)

LAMA_ROOT="."
DIRS=(
  "regression"
  "regression_long/deep-expressions"
  "regression_long/expressions"
)

BUILD_DIR="build"
mkdir -p "$BUILD_DIR"
pushd "$BUILD_DIR" >/dev/null
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j
if [[ ! -x "./lama_interpreter" ]]; then
  echo "Error: build did not produce lama_interpreter"
  popd >/dev/null
  exit 1
fi
LAMA_INTERPRETER="$(pwd)/lama_interpreter"
popd >/dev/null

OUT_DIR="output"
mkdir -p "$OUT_DIR"

total_tests=0
passed_tests=0

run_test() {
  local lama_file="$1"
  local base="$(basename "$lama_file" .lama)"
  local dir="$(dirname "$lama_file")"
  local input_file="$dir/$base.input"
  local bc_in_out="$OUT_DIR/$base.bc"
  local sol_file="$OUT_DIR/$base.sol"
  local out_file="$OUT_DIR/$base.out"
  local err_file="$OUT_DIR/$base.err"

  echo "running test $base"

  if [[ ! -f "$bc_in_out" ]]; then
    pushd "$OUT_DIR" >/dev/null
    if ! lamac -b "../$lama_file" >/dev/null; then
      echo "ERROR: lamac bytecode compilation failed for $lama_file"
      popd >/dev/null
      return 0
    fi
    popd >/dev/null
  fi

  if [[ ! -f "$sol_file" ]]; then
    if [[ -f "$input_file" ]]; then
      if ! lamac -i "$lama_file" < "$input_file" > "$sol_file" 2>/dev/null; then
        echo "ERROR: lamac -i failed for $lama_file"
      fi
    else
      if ! lamac -i "$lama_file" > "$sol_file" 2>/dev/null; then
        echo "ERROR: lamac -i failed for $lama_file (no input)"
      fi
    fi
  fi

  if [[ -f "$input_file" ]]; then
    if ! "$LAMA_INTERPRETER" "$bc_in_out" < "$input_file" > "$out_file" 2> "$err_file"; then
      rc=$?
      echo "ERROR: interpreter returned $rc for $lama_file"
    fi
  else
    if ! "$LAMA_INTERPRETER" "$bc_in_out" > "$out_file" 2> "$err_file"; then
      rc=$?
      echo "ERROR: interpreter returned $rc for $lama_file (no input)"
    fi
  fi

  if diff -q "$sol_file" "$out_file" >/dev/null 2>&1; then
    passed_tests=$((passed_tests + 1))
    echo "Ok"
  else
    echo "ERROR: output mismatch for $lama_file"
    echo "  diff $sol_file $out_file"
  fi
}

for rel in "${DIRS[@]}"; do
  test_dir="$LAMA_ROOT/$rel"
  if [[ ! -d "$test_dir" ]]; then
    echo "Warning: directory not found: $test_dir"
    continue
  fi
  shopt -s nullglob
  files=("$test_dir"/*.lama)
  for f in "${files[@]}"; do
    total_tests=$((total_tests + 1))
    run_test "$f"
  done
done

echo "Total tests: $total_tests"
echo "Passed: $passed_tests"
exit 0
