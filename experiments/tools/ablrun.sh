#!/bin/bash
for b in $(seq 0 17); do HP_OFF=$((1<<b)) ./abl.sh "off$b"; done
for lr in 2 4 10; do HP_LR=$lr ./abl.sh "lr$lr"; done
