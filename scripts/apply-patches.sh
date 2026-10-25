#!/usr/bin/env bash
# apply-patches.sh
#
# Applies the port's patches to the engine submodule. Safe to run repeatedly:
# a patch that is already applied is skipped.
#
# Usage: scripts/apply-patches.sh

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
engine_dir="${repo_root}/external/RSDKv5-Decompilation"

if [[ ! -f "${engine_dir}/CMakeLists.txt" ]]; then
    echo "Engine submodule missing. Run: git submodule update --init" >&2
    exit 1
fi

for patch in "${repo_root}"/patches/rsdk/*.patch; do
    name="$(basename "${patch}")"
    if git -C "${engine_dir}" apply --reverse --check "${patch}" >/dev/null 2>&1; then
        echo "already applied: ${name}"
    else
        git -C "${engine_dir}" apply "${patch}"
        echo "applied: ${name}"
    fi
done
