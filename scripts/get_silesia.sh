#!/usr/bin/env bash
# Download the Silesia corpus from a GitHub mirror and verify every file by SHA-256.
# Needs git and unzip.
# Usage: scripts/get_silesia.sh [dest_dir]   (default: corpus/silesia)
#        SILESIA_FILES="xml x-ray" scripts/get_silesia.sh dir   (only some files)
set -euo pipefail
dest="${1:-corpus/silesia}"
mirror="https://github.com/MiloszKrajewski/SilesiaCorpus.git"
files="${SILESIA_FILES:-dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml}"
mkdir -p "$dest"
declare -A sha=(
  [dickens]=b24c37886142e11d0ee687db6ab06f936207aa7f2ea1fd1d9a36763c7a507e6a
  [mozilla]=657fc3764b0c75ac9de9623125705831ebbfbe08fed248df73bc2dc66e2a963b
  [mr]=68637ed52e3e4860174ed2dc0840ac77d5f1a60abbcb13770d5754e3774d53e6
  [nci]=fc63a31770947b8c2062d3b19ca94c00485a232bb91b502021948fee983e1635
  [ooffice]=e7ee013880d34dd5208283d0d3d91b07f442e067454276095ded14f322a656eb
  [osdb]=60f027179302ca3ad87c58ac90b6be72ec23588aaa7a3b7fe8ecc0f11def3fa3
  [reymont]=0eac0114a3dfe6e2ee1f345a0f79d653cb26c3bc9f0ed79238af4933422b7578
  [samba]=93ba07bc44d8267789c1d911992f40b089ffa2140b4a160fac11ccae9a40e7b2
  [sao]=c2d0ea2cc59d4c21b7fe43a71499342a00cbe530a1d5548770e91ecd6214adcc
  [webster]=6a68f69b26daf09f9dd84f7470368553194a0b294fcfa80f1604efb11143a383
  [x-ray]=7de9fce1405dc44ae5e6813ed21cd5751e761bd4265655a005d39b9685d1c9ad
  [xml]=0e82e54e695c1938e4193448022543845b33020c8be6bf3bf3ead2224903e08c
)
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
need=""
for f in $files; do
  [ -f "$dest/$f" ] && echo "${sha[$f]}  $dest/$f" | sha256sum -c --quiet - >/dev/null 2>&1 || need="$need $f"
done
if [ -n "$need" ]; then  # sparse clone: fetch only the zips we need
  git clone -q --depth 1 --filter=blob:none --sparse "$mirror" "$tmp/m"
  git -C "$tmp/m" sparse-checkout set --no-cone $(for f in $need; do printf '/%s.zip ' "$f"; done)
fi
for f in $files; do
  if [ -f "$dest/$f" ] && echo "${sha[$f]}  $dest/$f" | sha256sum -c --quiet - >/dev/null 2>&1; then
    echo "ok      $f (already present)"; continue
  fi
  unzip -q -o "$tmp/m/$f.zip" -d "$tmp/x"
  mv "$tmp/x/$f" "$dest/$f"
  echo "${sha[$f]}  $dest/$f" | sha256sum -c --quiet - || { echo "HASH MISMATCH: $f"; exit 1; }
  echo "fetched $f"
done
