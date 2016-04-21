#!/bin/bash

# Benchmarks HEAD and HEAD~1 to identify performance regressions introduced
# in commits.
# TODO: should really be able to just compare two refspecs so we can e.g.
# compare the tip of a branch against the associated parent branch.

ROOT=$(git rev-parse --show-toplevel)

cd "$ROOT"

if ! git diff-index --quiet HEAD; then
    echo "benchmark.sh must modify the repository, but you have pending changes."
    # exit 0
fi

# Find the two refspecs to perform the comparison between.
PREV_REFSPEC="HEAD^"
CURR_REFSPEC="HEAD"
if [ "x$1" != "x" ]; then
    PREV_REFSPEC="$1"
fi
if [ "x$2" != "x" ]; then
    CURR_REFSPEC="$2"
fi

set -e

echo "Comparing benchmarks between $PREV_REFSPEC and $CURR_REFSPEC..."

BENCHMARK_COMMAND="./build/host/benchmarker --benchmark_format=json"
BENCHMARK_COMMAND="$BENCHMARK_COMMAND --benchmark_repetitions=5"

./easy_build_tests.sh
valgrind $BENCHMARK_COMMAND

exit 0

git pull

if [ "$PREV_REFSPEC" != "$CURR_REFSPEC" ]; then
    # Benchmark the previous commit.
    git checkout $PREV_REFSPEC

    ./easy_build_tests.sh
    $BENCHMARK_COMMAND >benchmark-PREV.json

    PREV="--prev=./benchmark-PREV.json"
    FORMAT="text"
else
    PREV=""
    FORMAT="json"
fi

# Benchmark the latest commit.
git checkout $CURR_REFSPEC

./easy_build_tests.sh
$BENCHMARK_COMMAND >benchmark-HEAD.json

# Analyze the results.
OUTPUT=$(python "$ROOT/scripts/analyze_benchmarks.py" $PREV \
    --current=./benchmark-HEAD.json --format=$FORMAT)

if [ "$PREV_REFSPEC" == "$CURR_REFSPEC" ]; then
    echo "Point-in-time benchmark on single refspec, uploading to Stackdriver"
    echo $OUTPUT | python "$ROOT/scripts/benchmarks_to_stackdriver.py"
else
    echo $OUTPUT
fi
