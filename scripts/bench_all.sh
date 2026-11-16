#!/usr/bin/env bash
# One command: build hp, fetch Silesia, run the round-trip benchmark, the timing
# head-to-head against gzip -6, and redraw the chart.
# Usage: scripts/bench_all.sh   (needs gcc, python3, git, unzip; pip install zstandard brotli lz4 matplotlib)
set -euo pipefail
cd "$(dirname "$0")/.."
gcc -O3 -march=native -o hp hp.c
bash scripts/get_silesia.sh corpus/silesia
python3 bench.py corpus/silesia gzip6,gzip9,lz4,zstd3,zstd9,zstd19,bzip2,xz9,brotli11,hp1,hp --out results.json
python3 timing.py corpus/silesia 10 gzip6,hp1 --out timing.json
python3 report.py results.json
python3 chart.py
