#!/usr/bin/env bash

set -e

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 kernel config_database" >&2
    exit 2
fi

kernel=$(realpath "$1")
configdb=$(realpath "$2")
smoke_dir=$(mktemp -d)
trap 'rm -rf "$smoke_dir"' EXIT

mkdir "$smoke_dir/empty-initrd"
(
    cd "$smoke_dir/empty-initrd"
    cmake -E tar cf "$smoke_dir/empty-initrd.tar" --format=gnutar -- .
)

log="$smoke_dir/kernel.log"
if ! (
    cd "$smoke_dir"
    timeout --signal=TERM 60s \
        "$kernel" "$smoke_dir/empty-initrd.tar" "$configdb" >"$log" 2>&1
); then
    cat "$log"
    echo "Hosted kernel did not complete successfully." >&2
    exit 1
fi

for checkpoint in \
    "Pedigree has started: all modules have been loaded." \
    "All modules unloaded. Running destructors and terminating..." \
    "main() returned, cleaning up..."
do
    if ! grep -aFq "$checkpoint" "$log"; then
        cat "$log"
        echo "Hosted kernel missed checkpoint: $checkpoint" >&2
        exit 1
    fi
    grep -aF "$checkpoint" "$log"
done

if grep -aFq "ERROR: AddressSanitizer" "$log"; then
    cat "$log"
    echo "AddressSanitizer reported an error." >&2
    exit 1
fi

echo "Hosted kernel smoke test passed."
