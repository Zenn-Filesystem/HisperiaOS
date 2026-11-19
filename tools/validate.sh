#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

for mode in 0 1 2 0; do
    echo "[VALIDATE] Building RUN_MPROTECT_TEST=${mode}"
    make clean
    make RUN_MPROTECT_TEST="${mode}" kernel
done

echo "[VALIDATE] All compile/link configurations passed."
echo "[VALIDATE] Runtime tests still require booting the ISO on hardware or QEMU."
