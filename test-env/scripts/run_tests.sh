#!/bin/bash
# ========================================================================
# Project: OpenRFStack
# Author:  Brendan Michaud
# Year:    2026
# Part of OpenRFStack (https://github.com/OpenRFStack)
#
# Licensed under the Personal Use License.
# Do not use for commercial, organizational, or military purposes.
# ========================================================================

set -e

BUILD_DIR="${BUILD_OUTPUT:-/build-output}"

# Build if not already built
if [ ! -f "${BUILD_DIR}/tests/sdr_tests" ]; then
    echo "[build] Configuring..."
    cmake -S /workspace -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release
    echo "[build] Compiling..."
    cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
fi

echo ""
echo "── Running GTest suite ──────────────────────────────────────────────"
ctest --test-dir "${BUILD_DIR}" --output-on-failure -V

# ========================================================================
# End of file — OpenRFStack
# Subject to Personal Use License
# https://github.com/OpenRFStack
# ========================================================================
