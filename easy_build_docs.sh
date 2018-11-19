#!/bin/bash

set -e

echo "Pedigree Easy Build"
echo "NOTE: This Easy Build script only generates documentation."

mkdir -p docs/doxygen
doxygen

