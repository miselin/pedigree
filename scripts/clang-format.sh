#!/bin/bash

set -euo pipefail

# clang-format accepts a file list, not a directory. Keeping enumeration here
# lets the repository ignore file own the exclusions without duplicating them in CI.
cd "$(git rev-parse --show-toplevel)"

file_list=$(mktemp)
trap 'rm -f "$file_list"' EXIT

git ls-files -- \
    'src/*.c' \
    'src/*.cc' \
    'src/*.cpp' \
    'src/*.cxx' \
    'src/*.h' \
    'src/*.hpp' > "$file_list"

clang-format --style=file --files="$file_list" "$@"
