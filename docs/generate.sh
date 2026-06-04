#!/usr/bin/env bash
# ========================================================================
# Project: OpenRFStack
# Author:  Brendan Michaud
# Year:    2026
# Part of OpenRFStack (https://github.com/OpenRFStack)
#
# Licensed under the Personal Use License.
# Do not use for commercial, organizational, or military purposes.
# ========================================================================

# generate.sh — Build Doxygen HTML documentation for the SDR stack.
#
# Run from the SdrResourceManager directory:
#   ./docs/generate.sh            # unified master docs (all repos)
#   ./docs/generate.sh single     # this repo only
#
# Uses system doxygen if available; otherwise falls back to a container.
# Outputs:
#   docs/master/html/index.html   (master mode)
#   docs/html/index.html          (single mode)

set -euo pipefail
cd "$(dirname "$0")/.."

MODE=${1:-master}
DOXYFILE="docs/Doxyfile.master"
OUT_DIR="docs/master/html"
if [ "$MODE" = "single" ]; then
    DOXYFILE="docs/Doxyfile"
    OUT_DIR="docs/html"
fi

run_doxygen() {
    if command -v doxygen &>/dev/null; then
        echo "[generate] Using system doxygen $(doxygen --version)"
        doxygen "$1"
    elif command -v podman &>/dev/null; then
        echo "[generate] Using doxygen container via podman"
        podman run --rm \
            -v "$(pwd)/..:/workspace:z" \
            -w "/workspace/SdrResourceManager" \
            docker.io/hrektts/doxygen \
            doxygen "$1"
    elif command -v docker &>/dev/null; then
        echo "[generate] Using doxygen container via docker"
        docker run --rm \
            -v "$(pwd)/..:/workspace" \
            -w "/workspace/SdrResourceManager" \
            hrektts/doxygen \
            doxygen "$1"
    else
        echo "[generate] ERROR: neither doxygen, podman, nor docker found."
        echo "  Install doxygen:  sudo dnf install doxygen  (Fedora/RHEL)"
        echo "                    sudo apt install doxygen  (Debian/Ubuntu)"
        exit 1
    fi
}

echo "[generate] Mode: $MODE"
echo "[generate] Doxyfile: $DOXYFILE"

run_doxygen "$DOXYFILE"

echo ""
echo "[generate] Done."
echo "[generate] Open: file://$(pwd)/$OUT_DIR/index.html"

# ========================================================================
# End of file — OpenRFStack
# Subject to Personal Use License
# https://github.com/OpenRFStack
# ========================================================================
