#!/bin/bash
set -e

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
exec "$script_dir/build-native.sh" aarch64 "$@"
