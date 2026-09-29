#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required. Install it from https://brew.sh/"
  exit 1
fi

brew install cmake ninja git docker
brew install --cask docker

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  export VCPKG_ROOT="${HOME}/vcpkg"
fi

if [[ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
  git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

"$VCPKG_ROOT/vcpkg" install --triplet x64-osx

cp -n .env.example .env 2>/dev/null || true
docker compose up -d redis

cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARKS=ON

cmake --build build
ctest --test-dir build --output-on-failure
