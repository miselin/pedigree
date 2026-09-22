#!/bin/bash

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P) && script_dir=$script_dir

cd ${script_dir}

pwd

mkdir -p build

docker build -t pedigree-alpine-rootfs .
exec docker run --rm \
    -v "$PWD/build:/out" \
    pedigree-alpine-rootfs
