#!/bin/bash

set -euo pipefail

# clang-format accepts a file list, not a directory. Keeping enumeration here
# lets the repository ignore file own the exclusions without duplicating them in CI.
cd "$(git rev-parse --show-toplevel)"

expected_version=$(cat .clang-format-version)
actual_version=$(clang-format --version | sed -E 's/^.* version ([0-9.]+).*$/\1/')
if [[ "$actual_version" != "$expected_version" ]]; then
    echo "clang-format $expected_version is required; found ${actual_version:-unknown}" >&2
    exit 1
fi

file_list=$(mktemp)
trap 'rm -f "$file_list"' EXIT

git ls-files -- \
    'src/*.c' \
    'src/*.cc' \
    'src/*.cpp' \
    'src/*.cxx' \
    'src/*.h' \
    'src/*.hpp' > "$file_list"

format_args=("$@")
dry_run=false
for arg in "${format_args[@]}"; do
    if [[ "$arg" == "--dry-run" ]]; then
        dry_run=true
        break
    fi
done
if [[ "$dry_run" == false ]]; then
    format_args=(-i "${format_args[@]}")
fi

clang-format --style=file --files="$file_list" "${format_args[@]}"
