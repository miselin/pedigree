#!/bin/bash

# Run clang-tidy over the source tree.
#
# This requires a compilation database - build one with
# -DCMAKE_EXPORT_COMPILE_COMMANDS=ON on the cmake command line.
#
# This is expected to be run from the root of the repository.

echo >tidy.log
if [ "x$SRCPATH" = "x" ]; then
    SRCPATH=src
fi
find $SRCPATH -regextype egrep -regex ".*\.(cc|c|cpp)$" -print0 | xargs -0 clang-tidy -p build >>tidy.log
