#!/usr/bin/env python3
"""SSL EQ plugin-level integration test (real CLAP plugin via axon_bench).

Covers the plugin<->engine glue that the C++ unit tests can't reach:
resolve_amount_ mapping SEQ_* -> SslEqParamsRT, the flush_chain SslEq case,
and the SEQ_ON master-bypass path. Complements tests/test_ssl_channel_eq.cpp
(engine + solver in isolation) and tests/test_control_contract.cpp (meta<->C++).

Requires a built plugin. Run after building:
    bash native/clap/build.sh axon "$PWD/weights/axon_bundle" "$PWD/build/Axon.clap"
    python3 native/clap/tests/test_ssl_integration.py

Exits 0 on pass, 1 on failure, 77 (skip) if the plugin/bench aren't built.
"""
import math, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CLAP = os.path.join(REPO, "build", "Axon.clap")
BENCH = os.path.join(HERE, "..", "build",
                     "axon_bench.exe" if os.name == "nt" else "axon_bench")

# Isolate the SSL EQ: every other orderable/parallel stage off.
COMMON = "MLI=0,RVB_MIX=0,WID_ON=0,BMI=0,AGN=0,SSC=0,EQ=0"


def skip(msg):
    print(f"SKIP: {msg}"); sys.exit(77)


def make_wav(path, sr=44100, n=None):
    n = n or sr
    import random
    random.seed(12345)
    frames = bytearray()
    for i in range(n):
        t = i / sr
        s = 0.3 * (random.random() * 2 - 1) + 0.25 * math.sin(2 * math.pi * 100 * t) \
            + 0.2 * math.sin(2 * math.pi * 1000 * t)
        s = max(-0.99, min(0.99, s))
        v = int(s * 32767)
        frames += struct.pack("<hh", v, v)
    with open(path, "wb") as f:
        import wave
        w = wave.open(f, "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes(bytes(frames)); w.close()


def read_left(path):
    b = open(path, "rb").read()
    i, data = 12, None
    while i + 8 <= len(b):
        cid = b[i:i + 4]; sz = struct.unpack("<I", b[i + 4:i + 8])[0]
        if cid == b"data":
            data = b[i + 8:i + 8 + sz]
        i += 8 + sz + (sz & 1)
    n = len(data) // 4
    s = struct.unpack("<%df" % n, data)
    return list(s[0::2])


def goertzel(x, f, sr=44100):
    k = 2 * math.cos(2 * math.pi * f / sr); s1 = s2 = 0.0
    for v in x:
        s0 = v + k * s1 - s2; s2 = s1; s1 = s0
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) / (len(x) / 2)


def run(inp, out, params):
    r = subprocess.run([BENCH, "--plugin", CLAP, "--in", inp, "--out", out,
                        "--params", params], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"axon_bench failed ({params}):\n{r.stderr}"); sys.exit(1)


def main():
    if not os.path.isdir(CLAP):
        skip(f"{CLAP} not built")
    if not os.access(BENCH, os.X_OK):
        skip(f"{BENCH} not built (needs libsndfile)")

    d = tempfile.mkdtemp(prefix="ssl_it_")
    inp = os.path.join(d, "in.wav")
    make_wav(inp)

    off_cranked = os.path.join(d, "off_cr.wav")
    off_flat = os.path.join(d, "off_fl.wav")
    on_cranked = os.path.join(d, "on_cr.wav")
    on_flat = os.path.join(d, "on_fl.wav")

    run(inp, off_cranked, f"{COMMON},SEQ_ON=0,SEQ_LF_G=12")
    run(inp, off_flat,    f"{COMMON},SEQ_ON=0,SEQ_LF_G=0")
    run(inp, on_cranked,  f"{COMMON},SEQ_ON=1,SEQ_LF_F=300,SEQ_LF_G=12")
    run(inp, on_flat,     f"{COMMON},SEQ_ON=1,SEQ_LF_F=300,SEQ_LF_G=0")

    A, B = read_left(off_cranked), read_left(off_flat)
    C, D = read_left(on_cranked), read_left(on_flat)

    # 1) SEQ_ON=0 is a master bypass: band settings must not change the output.
    d_off = max(abs(a - b) for a, b in zip(A, B))
    print(f"[ssl-it] SEQ_ON=0 bypass identity  max|d| = {d_off:.3e}")
    assert d_off == 0.0, "SEQ_ON=0 is not a bit-identical bypass"

    # 2) SEQ_ON=1 with a boosted LF shelf must audibly and spectrally change output.
    d_on = max(abs(c - dd) for c, dd in zip(C, D))
    assert d_on > 0.0, "SEQ_ON=1 LF boost produced no change"
    boost = 20 * math.log10(goertzel(C, 100) / goertzel(D, 100))
    print(f"[ssl-it] LF shelf +12 @corner300, measured @100Hz = {boost:.2f} dB")
    assert 6.0 < boost < 12.0, f"LF shelf boost {boost:.2f} dB out of expected range"

    print("ALL SSL INTEGRATION TESTS PASSED")


if __name__ == "__main__":
    main()
