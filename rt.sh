#!/bin/bash
# round-trip check: rt.sh [-1|-9] files...
lv=-9; case "$1" in -1|-9) lv=$1; shift;; esac
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
tot=0; totc=0
for f in "$@"; do
  s=$(date +%s.%N); ./pipr c $lv "$f" "$tmp/x.pipr" || { echo "$f COMPRESS FAIL"; exit 1; }
  m=$(date +%s.%N); ./pipr d "$tmp/x.pipr" "$tmp/x.out" || { echo "$f DECOMPRESS FAIL"; exit 1; }
  e=$(date +%s.%N)
  if cmp -s "$f" "$tmp/x.out"; then st=OK; else st=MISMATCH; fi
  o=$(stat -c%s "$f"); c=$(stat -c%s "$tmp/x.pipr"); tot=$((tot+o)); totc=$((totc+c))
  printf "%-12s %10d -> %9d  c %.2fs d %.2fs %s\n" $(basename "$f") $o $c $(echo "$m-$s"|bc) $(echo "$e-$m"|bc) $st
  [ $st = OK ] || exit 1
done
echo "TOTAL $tot -> $totc"
