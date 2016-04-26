#!/bin/bash

set -e

GIT_ROOT=$(git rev-parse --show-toplevel)

if [ ! -x "$GIT_ROOT/build/host/fuzz" ]; then
    echo "Fuzz tests are not present, not running."
    exit 0
fi

RUNS=1000000

mkdir -p "$GIT_ROOT/build/host/fuzz/out"

echo "Fuzzing memory functions..."
for f in "$GIT_ROOT"/build/host/fuzz/fuzz-Memory*; do
    echo $(basename $f)
    "$f" -runs=$RUNS "$GIT_ROOT/build/host/fuzz/out"
done

echo "Fuzzing String..."
for f in "$GIT_ROOT"/build/host/fuzz/fuzz-String*; do
    echo $(basename $f)
    "$f" -runs=$RUNS -only_ascii=1 "$GIT_ROOT/build/host/fuzz/out"
done
