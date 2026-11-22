#!/usr/bin/env python3
"""Turn results.json into a markdown table with ratios and Weissman scores.

Weissman score (Misra & Weissman, for HBO's Silicon Valley):
    W = alpha * (r / r_base) * (log T_base / log T)
r = compression ratio, T = compression time. alpha = 1, base = gzip -6.
The score depends on the time unit, so it is reported in seconds and ms.
"""
import json, math, sys

res = json.load(open(sys.argv[1] if len(sys.argv) > 1 else "results.json"))
order = ["lz4", "zstd3", "gzip6", "gzip9", "pipr1", "zstd9", "bzip2", "zstd19", "brotli11", "xz9", "pipr"]
label = {"lz4": "lz4 (default)", "zstd3": "zstd -3", "gzip6": "gzip -6", "gzip9": "gzip -9",
         "bzip2": "bzip2 -9", "zstd9": "zstd -9", "zstd19": "zstd -19", "brotli11": "brotli -11 (24-bit window)",
         "xz9": "xz -9", "pipr": "**pipr -9 (best)**", "pipr1": "**pipr -1 (fast)**"}
base = res["gzip6"]
rb, tb = base["total_orig"] / base["total_comp"], base["tc"]


def weiss(r, t, unit):
    tb_u, t_u = tb * unit, t * unit
    lt = math.log10(t_u)
    if lt == 0:
        return float("inf")
    return (r / rb) * math.log10(tb_u) / lt


print("| Compressor | Silesia size (bytes) | Ratio | vs xz -9 | Compress MB/s | Decompress MB/s | Weissman (s) | Weissman (ms) | Round-trip |")
print("|---|---:|---:|---:|---:|---:|---:|---:|---|")
xz = res["xz9"]["total_comp"]
for k in order:
    if k not in res:
        continue
    v = res[k]
    r = v["total_orig"] / v["total_comp"]
    mb = v["total_orig"] / 1e6
    print(f"| {label[k]} | {v['total_comp']:,} | {r:.3f} | {100*(v['total_comp']/xz-1):+.1f}% | "
          f"{mb/v['tc']:.2f} | {mb/v['td']:.2f} | {weiss(r, v['tc'], 1):.2f} | {weiss(r, v['tc'], 1000):.2f} | "
          f"{'12/12 exact' if v['all_exact'] else 'FAIL'} |")

print("\nPer file (compressed bytes):\n")
files = sorted(res["pipr"]["files"]) if "pipr" in res else []
cols = ["gzip6", "pipr1", "gzip9", "bzip2", "zstd19", "brotli11", "xz9", "pipr"]
print("| File | Original | " + " | ".join(label[c].replace("**", "") for c in cols) + " | pipr -9 vs best other |")
print("|---|---:|" + "---:|" * len(cols) + "---:|")
for f in files:
    row = [res[c]["files"][f]["comp"] for c in cols]
    best = min(row[2:-1])
    print(f"| {f} | {res['pipr']['files'][f]['orig']:,} | " + " | ".join(f"{x:,}" for x in row) +
          f" | {100*(row[-1]/best-1):+.1f}% |")
