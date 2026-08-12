#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pico2w_dir=$(dirname -- "$script_dir")
bluepad_dir="$pico2w_dir/external/bluepad32"
patch_file="$pico2w_dir/patches/bluepad32-runtime.patch"

if [ ! -e "$bluepad_dir/.git" ]; then
    echo "Bluepad32 submodule is not initialized." >&2
    echo "Run: git submodule update --init --recursive" >&2
    exit 1
fi

if git -c core.safecrlf=false -C "$bluepad_dir" apply --check \
        --ignore-space-change --ignore-whitespace "$patch_file" 2>/dev/null; then
    git -c core.safecrlf=false -C "$bluepad_dir" apply --whitespace=nowarn \
        --ignore-space-change --ignore-whitespace "$patch_file"
    echo "Applied the remotejoy-minus Bluepad32 runtime patch."
elif git -c core.safecrlf=false -C "$bluepad_dir" apply --reverse --check \
        --ignore-space-change --ignore-whitespace "$patch_file" 2>/dev/null; then
    echo "The remotejoy-minus Bluepad32 runtime patch is already applied."
else
    echo "Bluepad32 does not match the expected submodule revision," >&2
    echo "or it has conflicting local changes." >&2
    exit 1
fi
