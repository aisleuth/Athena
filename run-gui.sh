#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v cmake >/dev/null 2>&1; then
    echo "Athena requires CMake 3.25 or newer." >&2
    exit 1
fi
if ! command -v node >/dev/null 2>&1; then
    echo "Athena's GUI requires Node.js 18 or newer." >&2
    exit 1
fi

if [[ ! -x "${project_dir}/build/src/athena" ]]; then
    cmake -S "${project_dir}" -B "${project_dir}/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${project_dir}/build" --parallel
fi

exec node "${project_dir}/gui/server.mjs"
