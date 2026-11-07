#!/usr/bin/env bash
#
# Format all tracked C/C++ sources with clang-format. With no arguments it formats in place; arguments go straight to clang-format
# (CI runs: bash format.sh --dry-run --Werror). git ls-files leaves out external/ (submodule) and build/ (untracked).
#
# CI uses clang-format 21.1.8.
#
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format not found on PATH." >&2
    echo "  Linux:   sudo apt install clang-format   (or: pipx install clang-format==21.1.8)" >&2
    echo "  macOS:   brew install clang-format" >&2
    echo "  Windows: winget install LLVM.LLVM" >&2
    exit 1
fi

[ "$#" -gt 0 ] || set -- -i
git ls-files -z '*.cpp' '*.hpp' '*.h' | xargs -0 clang-format "$@"
