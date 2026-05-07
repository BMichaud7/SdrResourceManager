#!/usr/bin/env bash
# build.sh — clone owned sibling deps if missing, then build SdrResourceManager
#
# Owned dependencies cloned automatically:
#   ../SdrTaskApi   — https://github.com/BMichaud7/SdrTaskApi
#
# Third-party system packages (SoapySDR, qpid-proton, tinyxml2, fftw3, libuuid,
# spdlog, fmt) must already be installed.  CMake will print a FATAL_ERROR with
# the exact apt/dnf install command for anything that is missing.
#
# Usage:
#   ./build.sh                  # Release build
#   ./build.sh --debug          # Debug build (ASan + UBSan)
#   ./build.sh --clean          # Remove build/ then rebuild
#   ./build.sh --tests          # Build and run unit tests
#   ./build.sh --no-clone       # Skip the git-clone step (useful in CI)
#   ./build.sh --debug --tests  # Flags may be combined
#   ./build.sh --help

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "$REPO_DIR")"
BUILD_DIR="${REPO_DIR}/build"
BUILD_TYPE="Release"
RUN_TESTS=0
CLEAN=0
SKIP_CLONE=0

# ── Argument parsing ──────────────────────────────────────────────────────────
for arg in "$@"; do
    case "$arg" in
        --debug)        BUILD_TYPE="Debug" ;;
        --tests|-t)     RUN_TESTS=1 ;;
        --clean|-c)     CLEAN=1 ;;
        --no-clone)     SKIP_CLONE=1 ;;
        --help|-h)
            echo "Usage: $0 [--debug] [--clean] [--tests] [--no-clone]"
            echo ""
            echo "  --debug     Build with Debug mode (AddressSanitizer + UBSan)"
            echo "  --clean     Delete build/ before configuring"
            echo "  --tests     Build and run unit tests after a successful build"
            echo "  --no-clone  Skip cloning sibling repos (assume they exist)"
            echo ""
            echo "Owned dependencies cloned automatically to sibling directories:"
            echo "  ../SdrTaskApi  → https://github.com/BMichaud7/SdrTaskApi"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg  (use --help for usage)" >&2
            exit 1
            ;;
    esac
done

# ── Clone owned sibling repos if missing ─────────────────────────────────────
clone_if_missing() {
    local name="$1"
    local url="$2"
    local dest="$PARENT_DIR/$name"

    if [[ -d "$dest/.git" ]]; then
        echo "[deps] $name already present at $dest"
    elif [[ -d "$dest" ]]; then
        echo "[deps] $dest exists but is not a git repo — skipping clone"
    else
        echo "[deps] Cloning $name from $url → $dest"
        git clone --depth 1 "$url" "$dest"
        echo "[deps] $name cloned successfully"
    fi
}

if [[ $SKIP_CLONE -eq 0 ]]; then
    clone_if_missing "SdrTaskApi" "https://github.com/BMichaud7/SdrTaskApi.git"
fi

# ── Verify SdrTaskApi is reachable ────────────────────────────────────────────
SDRTASKAPI_DIR="$PARENT_DIR/SdrTaskApi"
if [[ ! -f "$SDRTASKAPI_DIR/CMakeLists.txt" ]]; then
    echo ""
    echo "ERROR: SdrTaskApi not found at $SDRTASKAPI_DIR" >&2
    echo "  Run without --no-clone, or:" >&2
    echo "  git clone https://github.com/BMichaud7/SdrTaskApi.git $SDRTASKAPI_DIR" >&2
    echo "" >&2
    exit 1
fi

# ── Clean ─────────────────────────────────────────────────────────────────────
if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "[SdrResourceManager] Removing existing build directory..."
    rm -rf "$BUILD_DIR"
fi

# ── Configure ─────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  SdrResourceManager build                                    ║"
echo "║  Build type  : $BUILD_TYPE                                   "
echo "║  SdrTaskApi  : $SDRTASKAPI_DIR                               "
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

cmake -B "$BUILD_DIR" -S "$REPO_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

# ── Build ─────────────────────────────────────────────────────────────────────
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo ""
echo "[SdrResourceManager] Build complete → $BUILD_DIR/sdr_controller"

# ── Tests ─────────────────────────────────────────────────────────────────────
if [[ $RUN_TESTS -eq 1 ]]; then
    echo ""
    echo "[SdrResourceManager] Running unit tests..."
    ctest --test-dir "$BUILD_DIR" --output-on-failure -V
    echo "[SdrResourceManager] All tests passed."
fi
