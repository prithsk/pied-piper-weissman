#!/usr/bin/env python3
"""Mutation fuzzer for hp's decoders. Run against a sanitizer build:

  gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o hp_asan hp.c
  python3 fuzz.py ./hp_asan 2000

Pass criteria for every mutated input:
  - exit code 0 or 2 only (2 = rejected as corrupt); no sanitizer report, signal or hang
  - exit code 0 only if the output equals the original (the CRC should make this certain)
"""
import os, random, subprocess, sys, tempfile, collections

BIN, N = sys.argv[1], int(sys.argv[2])
os.environ["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0:exitcode=99"
os.environ["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1:exitcode=98"
rng = random.Random(int(os.environ.get("FUZZ_SEED", "1234")))
TIMEOUT = 30


def seeds():
    text = open(os.path.join(os.path.dirname(__file__), "README.md"), "rb").read()
    xr = open(os.path.join(os.path.dirname(__file__), "../corpus/sil/x-ray"), "rb").read()[:30000]
    moz = open(os.path.join(os.path.dirname(__file__), "../corpus/sil/ooffice"), "rb").read()[:40000]
    return {
        "empty": b"", "one": b"x", "zeros": bytes(100000), "text": text, "xray": xr, "exe": moz,
        "pattern": b"abcabcabd" * 3000, "random": bytes(rng.getrandbits(8) for _ in range(5000)),
    }


def run(args, env=None):
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT, env=env)
        return p.returncode, p.stderr.decode(errors="replace")
    except subprocess.TimeoutExpired:
        return "timeout", ""


def mutate(blob):
    b = bytearray(blob)
    kind = rng.choice(["flip", "flip", "byte", "trunc", "extend", "header", "splice", "zero"])
    if kind == "flip" and b:
        for _ in range(rng.randint(1, 8)):
            i = rng.randrange(len(b)); b[i] ^= 1 << rng.randrange(8)
    elif kind == "byte" and b:
        for _ in range(rng.randint(1, 4)):
            b[rng.randrange(len(b))] = rng.getrandbits(8)
    elif kind == "trunc":
        b = b[:rng.randrange(len(b) + 1)]
    elif kind == "extend":
        b += bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 64)))
    elif kind == "header" and len(b) >= 13:
        i = rng.randrange(13)
        if 5 <= i < 9 and rng.random() < 0.7:  # size field: keep it small enough to finish
            v = int.from_bytes(b[5:9], "little")
            v = max(0, v + rng.randint(-5000, 50000))
            b[5:9] = v.to_bytes(4, "little")
        else:
            b[i] = rng.getrandbits(8)
    elif kind == "splice" and len(b) > 20:
        i, j = sorted(rng.randrange(13, len(b)) for _ in range(2))
        b = b[:i] + b[j:]
    elif kind == "zero" and len(b) > 13:
        i = rng.randrange(13, len(b)); b[i:i + 16] = bytes(len(b[i:i + 16]))
    return bytes(b), kind


def main():
    stats = collections.Counter()
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        comp = {}
        env = dict(os.environ, HP_TBITS="16")
        for name, data in seeds().items():
            for mode in ("-1", "-9"):
                src, dst = os.path.join(tmp, "s"), os.path.join(tmp, "s.hp")
                open(src, "wb").write(data)
                rc, err = run([BIN, "c", mode, src, dst], env)
                assert rc == 0, (name, mode, rc, err)
                comp[(name, mode)] = (data, open(dst, "rb").read())
        keys = sorted(comp)
        for k in keys:  # unmodified seeds must round-trip
            data, blob = comp[k]
            open(os.path.join(tmp, "f.hp"), "wb").write(blob)
            rc, err = run([BIN, "d", os.path.join(tmp, "f.hp"), os.path.join(tmp, "f.out")])
            assert rc == 0 and open(os.path.join(tmp, "f.out"), "rb").read() == data, (k, rc, err)
        # pure garbage with a valid-looking header
        for t in range(N):
            k = keys[t % len(keys)] if t < N - 50 else None
            if k is None:
                data = None
                blob = b"HP3" + bytes([rng.choice([0, 1, 2, 3]), 16]) + rng.randint(0, 200000).to_bytes(4, "little") \
                    + bytes(rng.getrandbits(8) for _ in range(4 + rng.randrange(3000)))
                kind = "garbage"
            else:
                data, blob = comp[k]
                blob, kind = mutate(blob)
            fin, fout = os.path.join(tmp, "f.hp"), os.path.join(tmp, "f.out")
            if os.path.exists(fout): os.remove(fout)
            open(fin, "wb").write(blob)
            rc, err = run([BIN, "d", fin, fout])
            if rc == 0:
                ok = data is not None and open(fout, "rb").read() == data
                stats["accepted_identical" if ok else "ACCEPTED_WRONG"] += 1
                if not ok: bad.append((k, kind, "accepted wrong output"))
            elif rc == 2:
                stats["rejected"] += 1
                if os.path.exists(fout): bad.append((k, kind, "wrote output despite rejecting"))
            else:
                stats[f"CRASH_{rc}"] += 1
                bad.append((k, kind, rc, err[-400:]))
                open(os.path.join(os.path.dirname(os.path.abspath(__file__)), f"crash_{t}.hp"), "wb").write(blob)
            if (t + 1) % 250 == 0:
                print(t + 1, dict(stats), flush=True)
    print("FINAL", dict(stats))
    for b in bad[:20]:
        print("BAD", b)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
