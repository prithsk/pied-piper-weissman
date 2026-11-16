#!/bin/bash
# lzgrid.sh label -Dflags...   -> total size and compress time over full Silesia (median of 2)
lab=$1; shift
gcc -O3 -march=native "$@" -o /tmp/hpx ${SRC:-hp.c} || exit 1
best=999
for r in 1 2; do
  s=$(date +%s.%N); tot=0
  for f in ../corpus/sil/*; do /tmp/hpx c -1 $f /tmp/g.hp; tot=$((tot+$(stat -c%s /tmp/g.hp))); done
  e=$(echo "$(date +%s.%N)-$s"|bc); best=$(echo "if($e<$best) $e else $best"|bc)
done
echo "$lab $tot ${best}s"
