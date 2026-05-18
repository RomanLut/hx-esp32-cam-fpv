#!/usr/bin/env bash
set -euo pipefail
# Run from Linux/WSL directly (no Windows .bat wrapper needed).
# Requires: curl and a C++ toolchain (build-essential).
# CMake is bootstrapped automatically if missing (see build_linux.sh).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CONFIG=Release exec bash "${REPO_ROOT}/OpenCV/OpenCVWrapper/scripts/build_linux.sh" "$@"
