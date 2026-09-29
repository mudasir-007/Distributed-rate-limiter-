#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== Distributed Rate Limiter / Linux setup ==="

if ! command -v sudo >/dev/null 2>&1; then
  echo "sudo is required for the dependency installation step."
  exit 1
fi

sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git curl pkg-config docker.io docker-compose-plugin

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker installation failed."
  exit 1
fi

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  export VCPKG_ROOT="${HOME}/vcpkg"
fi

if [[ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
  git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

"$VCPKG_ROOT/vcpkg" install --triplet x64-linux

cp -n .env.example .env 2>/dev/null || true

docker compose up -d redis

until docker inspect -f '{{.State.Health.Status}}' distributed-rate-limiter-redis 2>/dev/null | grep -q healthy; do
  sleep 1
done

cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release

echo ""
echo "Setup complete."
echo "Run demo: ./scripts/run_demo.sh"
echo "Run benchmark: ./build/bench 16 10000 1000"
echo "Run Redis benchmark: ./build/bench_redis"
