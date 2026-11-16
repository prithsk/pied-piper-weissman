#!/usr/bin/env python3
"""lzcpu.py label [-Dflags...]: build hp.c with flags, compress Silesia with -1 three times,
report total size and the minimum total CPU time (user+sys) across runs."""
import os, subprocess, sys, resource, glob
lab, flags = sys.argv[1], sys.argv[2:]
src = os.environ.get("SRC", "hp.c")
subprocess.run(["gcc", "-O3", "-march=native", *flags, "-o", "/tmp/hpx", src], check=True)
files = sorted(glob.glob("../corpus/sil/*"))
best = 1e9
for r in range(3):
    t0 = resource.getrusage(resource.RUSAGE_CHILDREN); size = 0
    for f in files:
        subprocess.run(["/tmp/hpx", "c", "-1", f, "/tmp/g.hp"], check=True)
        size += os.path.getsize("/tmp/g.hp")
    t1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    best = min(best, (t1.ru_utime - t0.ru_utime) + (t1.ru_stime - t0.ru_stime))
print(f"{lab:14s} {size:,} bytes  cpu {best:.2f}s")
