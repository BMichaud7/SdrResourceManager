#!/bin/bash
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
