#!/usr/bin/env python3
"""Draw docs/benchmark.png: compression ratio vs compression speed on Silesia.

Uses results.json for every tool and timing.json medians for gzip -6 and hp -1.
Usage: python3 chart.py
"""
import json, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker

here = os.path.dirname(os.path.abspath(__file__))
res = json.load(open(os.path.join(here, "results.json")))
tim = json.load(open(os.path.join(here, "timing.json")))["results"]

label = {"lz4": "lz4", "zstd3": "zstd -3", "gzip6": "gzip -6", "gzip9": "gzip -9", "bzip2": "bzip2 -9",
         "zstd9": "zstd -9", "zstd19": "zstd -19", "brotli11": "brotli -11", "xz9": "xz -9", "hp1": "hp -1", "hp": "hp -9"}
pts = []
for k, name in label.items():
    v = res[k]
    tc = tim[k]["tc_med"] if k in tim else v["tc"]
    pts.append((name, v["total_orig"] / 1e6 / tc, v["total_orig"] / v["total_comp"], k.startswith("hp")))

SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE = "#2a78d6", "#eb6834"
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11})
fig, ax = plt.subplots(figsize=(10, 6), dpi=160)
fig.patch.set_facecolor(SURFACE); ax.set_facecolor(SURFACE)

for hp_series, color, marker, size, name in ((False, BLUE, "o", 70, "Standard tools"), (True, ORANGE, "D", 95, "hp")):
    xs = [p[1] for p in pts if p[3] == hp_series]; ys = [p[2] for p in pts if p[3] == hp_series]
    ax.scatter(xs, ys, s=size, c=color, marker=marker, edgecolors=SURFACE, linewidths=2, zorder=3, label=name)

offsets = {"hp -9": (10, -4), "hp -1": (-56, -4), "gzip -6": (10, -12), "gzip -9": (-58, 6), "xz -9": (10, -4),
           "brotli -11": (10, -4), "zstd -19": (10, -4), "zstd -9": (8, 6), "bzip2 -9": (10, -4), "zstd -3": (10, -4), "lz4": (-30, 8)}
for name, x, y, is_hp in pts:
    ax.annotate(name, (x, y), xytext=offsets.get(name, (8, -4)), textcoords="offset points",
                color=INK, fontsize=11, fontweight="bold" if is_hp else "normal")

ax.set_xscale("log")
ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
ax.set_xlabel("Compression speed, MB/s (log scale, single thread)", color=INK2)
ax.set_ylabel("Compression ratio (higher is smaller)", color=INK2)
ax.set_title("hp vs standard compressors on the Silesia corpus (212 MB)", color=INK, loc="left", fontsize=14, pad=14)
ax.grid(True, color=GRID, linewidth=0.8, zorder=0); ax.set_axisbelow(True)
for s in ("top", "right"): ax.spines[s].set_visible(False)
for s in ("left", "bottom"): ax.spines[s].set_color(GRID)
ax.tick_params(colors=INK2)
ax.set_ylim(1.8, 5.8)
ax.annotate("better", xy=(0.97, 0.96), xytext=(0.84, 0.80), xycoords="axes fraction", color=INK2, fontsize=10,
            arrowprops=dict(arrowstyle="->", color=INK2, lw=1.2), ha="center")
leg = ax.legend(loc="lower left", frameon=False, labelcolor=INK)
fig.text(0.01, 0.01, "Every point round-trips all 12 files byte-exact. 2-core Xeon @ 2.1 GHz. "
         "gzip -6 and hp -1 are medians of 10 runs.", color=INK2, fontsize=8.5)
fig.tight_layout(rect=(0, 0.03, 1, 1))
os.makedirs(os.path.join(here, "docs"), exist_ok=True)
fig.savefig(os.path.join(here, "docs", "benchmark.png"), facecolor=SURFACE)
print("wrote docs/benchmark.png")
