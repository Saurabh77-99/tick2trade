#!/usr/bin/env bash
# Build, test, generate a synthetic day, replay it, and open the viewer.
#   ./scripts/demo.sh            then open http://localhost:8080
set -euo pipefail
cd "$(dirname "$0")/.."

jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$jobs"
ctest --test-dir build --output-on-failure

mkdir -p data
[ -f data/synthetic.itch ] || ./build/itch gen data/synthetic.itch --messages 5000000 --symbols 12
./build/itch replay data/synthetic.itch --warm --symbol AAPL --depth 5 --check
./build/itch snapshot data/synthetic.itch --symbol AAPL --out data/AAPL.jsonl

cd viewer
[ -d node_modules ] || npm install --silent
exec node server.js ../data/AAPL.jsonl --port "${PORT:-8080}"
