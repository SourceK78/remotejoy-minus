#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=${1:-"$script_dir/../external/joypad-os"}
patch_dir="$script_dir/../patches"

for patch in "$patch_dir"/*.patch; do
    if git -C "$repo" apply --recount --reverse --check "$patch" >/dev/null 2>&1; then
        printf '%s\n' "Already applied: $(basename "$patch")"
    else
        git -C "$repo" apply --recount --check "$patch"
        git -C "$repo" apply --recount "$patch"
        printf '%s\n' "Applied: $(basename "$patch")"
    fi
done
