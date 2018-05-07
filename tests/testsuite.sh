#!/bin/bash

set -e

GIT_ROOT=$(git rev-parse --show-toplevel)

# Run the Python-based testsuites.
/usr/bin/env python -m unittest discover -s $GIT_ROOT/tests -p "*_tests.py" -v -f -c

# Remove old coverage data.
find "$GIT_ROOT" -type f -name '*.gcda' -delete

cd "$GIT_ROOT/build-host"

# Run testsuites and then collect coverage data if it passes.
ctest
make testsuite_coverage

# Run the benchmarks (useful for discovering performance regressions).
# TODO(miselin): figure out a way to tracking that the benchmark changed
# to a relatively large extent.
if [ -x "$GIT_ROOT/build-host/src/buildutil/benchmarker" ]; then
    # NOTE: we don't run this under Valgrind - that doesn't make sense in this
    # case (as we'd be not testing true performance).
    "$GIT_ROOT/build-host/src/buildutil/benchmarker"
fi

# Coverage failing is not a problem; but make sure to filter only to src/ so
# we don't pull in e.g. all of the gtest code.
lcov --directory . --capture --output-file testsuite_coverage.info || exit 0
lcov --remove testsuite_coverage.info '/usr/*' --output-file testsuite_coverage.info || exit 0
lcov --list coverage.info || exit 0
