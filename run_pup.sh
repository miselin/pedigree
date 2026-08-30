#!/bin/bash

# Run pup, and install pup if it is not present.

DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )

LOCAL_PUP=1

if [[ ${LOCAL_PUP} == 1 ]]; then
    # Install the sibling pedigree-apps pup editably if not already present
    repo_dir="$(cd "${DIR}/../pedigree-apps/pup" && pwd)"
    module="pedigree_updater"

    module_path="$(
      uv run python -c "
import importlib.util
spec = importlib.util.find_spec('$module')
print(spec.origin or '' if spec else '')
"
)"

    if [[ "$module_path" != "$repo_dir/"* ]]; then
        echo "Installing local editable package ($module_path != $repo_dir)..."
        (
            uv pip install -e "$repo_dir"
        )
    fi

    exec uv run -m "$module" --config="$DIR/scripts/pup/pup.conf" "$@"
fi

PUP_TMP="$DIR/scripts/pup.whl.tmp"
PUP="$DIR/scripts/pup.whl"

set +e

try_update_pup()
{
    curl -o ".pup-version-new" https://pup.pedigree-project.org/pup-version
    if cmp --silent ".pup-version-new" ".pup-version"; then
        rm -f ".pup-version-new"
    else
        curl -o "$PUP_TMP" https://pup.pedigree-project.org/pup.whl && \
            mv "$PUP_TMP" "$PUP" && mv ".pup-version-new" ".pup-version"
    fi
}

# Download pup for the first time if we don't know what version we have.
if [ ! -e .pup-version ]; then
    try_update_pup
fi

# Update pup if needed.
if test $(find .pup-version -maxdepth 1 -mmin +120); then
    try_update_pup
fi

set -e

exec python3 "$PUP/pedigree_updater" --config="$DIR/scripts/pup/pup.conf" $*

