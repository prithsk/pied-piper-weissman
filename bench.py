#!/usr/bin/env python3
"""Benchmark harness. Hard gate: every file must round-trip byte-exact.

Usage: bench.py <corpus_dir> <compressor,...> [--out results.json]
Compressors: gzip6 gzip9 bzip2 xz6 xz9 zstd3 zstd19 brotli11 lz4 hp (-9) hp1 (-1)
"""
import hashlib, json, os, subprocess, sys, time, tempfile, math

HP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hp")


def cli(cmd_c, cmd_d):
    def c(data, tmp):
        t = time.perf_counter()
        out = subprocess.run(cmd_c, input=data, stdout=subprocess.PIPE, check=True).stdout
        return out, time.perf_counter() - t
    def d(blob, tmp):
        t = time.perf_counter()
        out = subprocess.run(cmd_d, input=blob, stdout=subprocess.PIPE, check=True).stdout
        return out, time.perf_counter() - t
    return c, d


def lib(fc, fd):
    def c(data, tmp):
        t = time.perf_counter(); out = fc(data); return out, time.perf_counter() - t
    def d(blob, tmp):
        t = time.perf_counter(); out = fd(blob); return out, time.perf_counter() - t
    return c, d


def hp_tool(extra=()):
    def c(data, tmp):
        src, dst = os.path.join(tmp, "in"), os.path.join(tmp, "in.hp")
        open(src, "wb").write(data)
        t = time.perf_counter()
        subprocess.run([HP, "c", *extra, src, dst], check=True)
        el = time.perf_counter() - t
        return open(dst, "rb").read(), el
    def d(blob, tmp):
        src, dst = os.path.join(tmp, "x.hp"), os.path.join(tmp, "x.out")
        open(src, "wb").write(blob)
        t = time.perf_counter()
        subprocess.run([HP, "d", src, dst], check=True)
        el = time.perf_counter() - t
        return open(dst, "rb").read(), el
    return c, d


def make(name):
    if name.startswith("gzip"):
        lv = name[4:]; return cli(["gzip", f"-{lv}", "-c"], ["gzip", "-dc"])
    if name == "bzip2":
        return cli(["bzip2", "-9", "-c"], ["bzip2", "-dc"])
    if name.startswith("xz"):
        lv = name[2:]; return cli(["xz", f"-{lv}", "-T1", "-c"], ["xz", "-dc"])
    if name.startswith("zstd"):
        import zstandard as z
        lv = int(name[4:])
        return lib(lambda b: z.ZstdCompressor(level=lv).compress(b),
                   lambda b: z.ZstdDecompressor().decompress(b))
    if name.startswith("brotli"):
        import brotli
        q = int(name[6:])
        return lib(lambda b: brotli.compress(b, quality=q, lgwin=24), brotli.decompress)
    if name == "lz4":
        import lz4.frame as l
        return lib(l.compress, l.decompress)
    if name == "hp":
        return hp_tool()
    if name == "hp1":
        return hp_tool(("-1",))
    raise SystemExit("unknown compressor " + name)


def main():
    corpus, names = sys.argv[1], sys.argv[2].split(",")
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    files = sorted(f for f in os.listdir(corpus) if os.path.isfile(os.path.join(corpus, f)))
    results = json.load(open(out)) if out and os.path.exists(out) else {}
    for name in names:
        c, d = make(name)
        rows, ok = {}, True
        for f in files:
            data = open(os.path.join(corpus, f), "rb").read()
            with tempfile.TemporaryDirectory() as tmp:
                blob, tc = c(data, tmp)
                back, td = d(blob, tmp)
            exact = hashlib.sha256(back).digest() == hashlib.sha256(data).digest()
            ok &= exact
            rows[f] = dict(orig=len(data), comp=len(blob), tc=tc, td=td, exact=exact)
            print(f"{name:9s} {f:10s} {len(data):>10d} -> {len(blob):>10d}  "
                  f"{len(data)/len(blob):6.3f}x  c {tc:7.2f}s  d {td:7.2f}s  "
                  f"{'OK' if exact else 'MISMATCH'}", flush=True)
        tot_o = sum(r["orig"] for r in rows.values()); tot_c = sum(r["comp"] for r in rows.values())
        tc = sum(r["tc"] for r in rows.values()); td = sum(r["td"] for r in rows.values())
        print(f"{name:9s} TOTAL      {tot_o:>10d} -> {tot_c:>10d}  {tot_o/tot_c:6.3f}x  "
              f"c {tc:7.2f}s  d {td:7.2f}s  {'ALL OK' if ok else 'FAILED'}\n", flush=True)
        results[name] = dict(files=rows, total_orig=tot_o, total_comp=tot_c, tc=tc, td=td, all_exact=ok)
        if out:
            json.dump(results, open(out, "w"), indent=1)
        if not ok:
            sys.exit(f"ROUND-TRIP FAILURE in {name}")


if __name__ == "__main__":
    main()
