#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -f .env ]]; then
  cp .env.example .env
  echo "Created .env from .env.example"
fi

set -a
# shellcheck disable=SC1091
source .env
set +a

docker compose up -d redis
until docker inspect -f '{{.State.Health.Status}}' distributed-rate-limiter-redis 2>/dev/null | grep -q healthy; do
  sleep 1
done

if [[ ! -f build/CMakeCache.txt ]]; then
  cmake --preset linux-release
fi
cmake --build --preset linux-release --target concurrency_demo

export RATE_LIMITER_SCRIPT_DIR="$ROOT/scripts"
./build/concurrency_demo
