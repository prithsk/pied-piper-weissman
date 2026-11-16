#!/usr/bin/env bash
# What CI runs: build with warnings, round-trip both modes, reject corrupt input,
# then fuzz both decoders under AddressSanitizer + UndefinedBehaviorSanitizer.
# Usage: scripts/ci_check.sh [fuzz_cases]   (default 300)
set -euo pipefail
cd "$(dirname "$0")/.."
cases="${1:-300}"
SILESIA_FILES="xml x-ray ooffice" bash scripts/get_silesia.sh corpus/silesia
gcc -O2 -march=native -Wall -Wextra -Wno-misleading-indentation -Werror -o hp hp.c
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
head -c 1000000 corpus/silesia/x-ray > "$tmp/xray1m"
head -c 1000000 corpus/silesia/ooffice > "$tmp/ooffice1m"
: > "$tmp/empty"; printf 'x' > "$tmp/one"
for f in corpus/silesia/xml corpus/silesia/x-ray corpus/silesia/ooffice "$tmp/empty" "$tmp/one" README.md; do
  ./hp c -1 "$f" "$tmp/a.hp"; ./hp d "$tmp/a.hp" "$tmp/a.out"; cmp "$f" "$tmp/a.out"
  echo "round trip -1 ok: $(basename "$f")"
done
for f in corpus/silesia/xml "$tmp/xray1m" "$tmp/ooffice1m" "$tmp/empty" "$tmp/one"; do
  HP_TBITS=20 ./hp c "$f" "$tmp/b.hp"; ./hp d "$tmp/b.hp" "$tmp/b.out"; cmp "$f" "$tmp/b.out"
  echo "round trip -9 ok: $(basename "$f")"
done
# a flipped byte must be rejected with exit code 2 and no output file
cp "$tmp/a.hp" "$tmp/bad.hp"; printf '\xff' | dd of="$tmp/bad.hp" bs=1 seek=20 conv=notrunc 2>/dev/null
rm -f "$tmp/bad.out"; set +e; ./hp d "$tmp/bad.hp" "$tmp/bad.out" 2>/dev/null; rc=$?; set -e
[ "$rc" = 2 ] && [ ! -e "$tmp/bad.out" ] && echo "corrupt input rejected ok"
# same input and output must be refused without touching the file
cp README.md "$tmp/same"; set +e; ./hp c "$tmp/same" "$tmp/same" 2>/dev/null; rc=$?; set -e
[ "$rc" = 1 ] && cmp README.md "$tmp/same" && echo "same-file guard ok"
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -march=native -o hp_asan hp.c
FUZZ_CORPUS=corpus/silesia python3 fuzz.py ./hp_asan "$cases"
echo "ALL CHECKS PASSED"
