#!/bin/bash
# abl.sh label  (env HP_* passed through) -> total compressed bytes over ../mix, compress only
tot=0
for f in ../mix/*; do ${HPBIN:-./hp} c $f /tmp/a.$$.hp; tot=$((tot+$(stat -c%s /tmp/a.$$.hp))); done
echo "$1 $tot"
