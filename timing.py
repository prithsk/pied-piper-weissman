#!/usr/bin/env python3
"""Repeated, interleaved timing of compressors on a corpus, with the round-trip gate on every run.

Usage: timing.py <corpus_dir> <runs> <compressor,...> [--out timing.json]
Reports the median total compress/decompress time per compressor and the Weissman score
against gzip -6 (alpha = 1) in seconds and in milliseconds.
"""
import json, math, os, statistics, sys, tempfile, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench import make

corpus, runs, names = sys.argv[1], int(sys.argv[2]), sys.argv[3].split(",")
out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
files = sorted(f for f in os.listdir(corpus) if os.path.isfile(os.path.join(corpus, f)))
data = {f: open(os.path.join(corpus, f), "rb").read() for f in files}
tools = {n: make(n) for n in names}
res = {n: dict(tc=[], td=[], size=None) for n in names}
for r in range(runs):
    order = names if r % 2 == 0 else names[::-1]  # alternate order to spread drift evenly
    for n in order:
        c, d = tools[n]; tc = td = 0.0; size = 0
        for f in files:
            with tempfile.TemporaryDirectory() as tmp:
                blob, t1 = c(data[f], tmp)
                back, t2 = d(blob, tmp)
            if hashlib.sha256(back).digest() != hashlib.sha256(data[f]).digest():
                sys.exit(f"ROUND-TRIP FAILURE: {n} {f}")
            tc += t1; td += t2; size += len(blob)
        res[n]["tc"].append(tc); res[n]["td"].append(td); res[n]["size"] = size
        print(f"run {r+1} {n:7s} size {size:>11,d} compress {tc:6.2f}s decompress {td:6.2f}s", flush=True)

orig = sum(len(v) for v in data.values())
base = res.get("gzip6")
print()
for n in names:
    v = res[n]; tc, td = statistics.median(v["tc"]), statistics.median(v["td"])
    v.update(tc_med=tc, td_med=td, ratio=orig / v["size"])
    line = f"{n:7s} ratio {orig/v['size']:.3f}  compress median {tc:.2f}s (min {min(v['tc']):.2f}, max {max(v['tc']):.2f})  decompress median {td:.2f}s"
    if base:
        rb, tb = orig / base["size"], statistics.median(base["tc"])
        w = lambda u: (orig / v["size"] / rb) * math.log10(tb * u) / math.log10(tc * u)
        v.update(w_s=w(1), w_ms=w(1000))
        line += f"  W(s) {w(1):.3f}  W(ms) {w(1000):.3f}"
        if n != "gzip6":
            dom = v["size"] < base["size"] and max(v["tc"]) < min(base["tc"])
            line += "  smaller AND faster than gzip -6 on every run" if dom else ""
    print(line)
if out:
    json.dump(dict(orig=orig, runs=runs, results=res), open(out, "w"), indent=1)
