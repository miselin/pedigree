#!/bin/bash

# Run pup, and install pup if it is not present.

DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )

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
