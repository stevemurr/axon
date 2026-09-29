#!/usr/bin/env python3
"""SSL EQ plugin-level integration test (real CLAP plugin via axon_bench).

Covers the plugin<->engine glue that the C++ unit tests can't reach:
resolve_amount_ mapping SEQ_* -> SslEqParamsRT, the flush_chain SslEq case,
the SEQ_ON master-bypass path, independent Stereo/Mid/Side bank routing,
SEQ_MODE's editor-only behavior, and the SEQ_TYPE voicing switch. Complements
tests/test_ssl_channel_eq.cpp (engine + solver in isolation) and
tests/test_control_contract.cpp (meta<->C++).

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


def make_wav(path, sr=44100, n=None, side=False, clean=False, tones=None):
    n = n or sr
    import random
    random.seed(12345)
    frames = bytearray()
    for i in range(n):
        t = i / sr
        if tones:
            s = sum(a * math.sin(2 * math.pi * f * t) for f, a in tones)
        else:
            s = 0.2 * math.sin(2 * math.pi * 1000 * t) if clean else \
                0.3 * (random.random() * 2 - 1) + 0.25 * math.sin(2 * math.pi * 100 * t) \
                + 0.2 * math.sin(2 * math.pi * 1000 * t)
        s = max(-0.99, min(0.99, s))
        v = int(s * 32767)
        frames += struct.pack("<hh", v, -v if side else v)
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
    mid_boost = os.path.join(d, "mid_boost.wav")
    side_on_mid = os.path.join(d, "side_on_mid.wav")
    combo_in = os.path.join(d, "combo_in.wav")
    combo_flat = os.path.join(d, "combo_flat.wav")
    stereo_mid_combo = os.path.join(d, "stereo_mid_combo.wav")
    make_wav(combo_in, clean=True)

    run(inp, off_cranked, f"{COMMON},SEQ_ON=0,SEQ_LF_G=12")
    run(inp, off_flat,    f"{COMMON},SEQ_ON=0,SEQ_LF_G=0")
    run(inp, on_cranked,  f"{COMMON},SEQ_ON=1,SEQ_LF_F=300,SEQ_LF_G=12")
    run(inp, on_flat,     f"{COMMON},SEQ_ON=1,SEQ_LF_F=300,SEQ_LF_G=0")
    run(inp, mid_boost,   f"{COMMON},SEQ_ON=1,SEQ_MODE=0,SEQ_MID_LMF_F=1000,SEQ_MID_LMF_G=9")
    run(inp, side_on_mid, f"{COMMON},SEQ_ON=1,SEQ_MODE=1,SEQ_SIDE_LMF_F=1000,SEQ_SIDE_LMF_G=9")
    run(combo_in, combo_flat, f"{COMMON},SEQ_ON=1,SEQ_LMF_G=0,SEQ_MID_LMF_G=0")
    run(combo_in, stereo_mid_combo,
        f"{COMMON},SEQ_ON=1,SEQ_MODE=2,SEQ_LMF_F=1000,SEQ_LMF_G=3,SEQ_MID_LMF_F=1000,SEQ_MID_LMF_G=6")

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

    # 3) Mid-only input: its independent Mid bank changes it, Side stays transparent.
    # SEQ_MODE deliberately differs between renders: it is editor state, not routing.
    M = read_left(mid_boost)
    S_on_M = read_left(side_on_mid)
    CF = read_left(combo_flat)
    SM = read_left(stereo_mid_combo)
    d_mid = max(abs(m - dd) for m, dd in zip(M, D))
    d_side_on_mid = max(abs(s - dd) for s, dd in zip(S_on_M, D))
    print(f"[ssl-it] mid input: Mid max|d|={d_mid:.3e}, Side max|d|={d_side_on_mid:.3e}")
    assert d_mid > 0.0, "independent Mid bank did not process mid content"
    assert d_side_on_mid < 1e-12, "independent Side bank changed pure-mid content"
    combo_gain = 20 * math.log10(goertzel(SM, 1000) / goertzel(CF, 1000))
    print(f"[ssl-it] simultaneous Stereo +3 / Mid +6 @1k = {combo_gain:.2f} dB")
    assert 8.5 < combo_gain < 9.5, "Stereo and Mid banks did not cascade independently"

    # 4) Pure-side input: its independent Side bank changes it, Mid stays transparent.
    side_inp = os.path.join(d, "side_in.wav")
    side_flat = os.path.join(d, "side_flat.wav")
    mid_on_side = os.path.join(d, "mid_on_side.wav")
    side_boost = os.path.join(d, "side_boost.wav")
    make_wav(side_inp, side=True)
    run(side_inp, side_flat,   f"{COMMON},SEQ_ON=1,SEQ_MODE=2,SEQ_SIDE_LMF_G=0")
    run(side_inp, mid_on_side, f"{COMMON},SEQ_ON=1,SEQ_MODE=0,SEQ_MID_LMF_F=1000,SEQ_MID_LMF_G=9")
    run(side_inp, side_boost,  f"{COMMON},SEQ_ON=1,SEQ_MODE=1,SEQ_SIDE_LMF_F=1000,SEQ_SIDE_LMF_G=9")
    SF = read_left(side_flat)
    M_on_S = read_left(mid_on_side)
    SB = read_left(side_boost)
    d_mid_on_side = max(abs(m - f) for m, f in zip(M_on_S, SF))
    d_side = max(abs(s - f) for s, f in zip(SB, SF))
    print(f"[ssl-it] side input: Mid max|d|={d_mid_on_side:.3e}, Side max|d|={d_side:.3e}")
    assert d_mid_on_side < 1e-12, "independent Mid bank changed pure-side content"
    assert d_side > 0.0, "independent Side bank did not process side content"

    # 5) SEQ_TYPE (Classic=0 / Broad=1) reaches all banks. Flat Broad matches flat
    # Classic (Broad's flat sections are exact identities; Classic's 0 dB RBJ
    # sections carry ~1e-15 rounding); with bands dialled in, the same knobs land
    # the same peak (+9 dB at 1 kHz) but Broad's console bell is much wider
    # (model: +0.87 dB at 100 Hz vs Classic's +0.11 dB).
    broad_flat = os.path.join(d, "broad_flat.wav")
    tones_in = os.path.join(d, "tones_in.wav")       # quiet: stays clear of the ceiling
    tones_flat = os.path.join(d, "tones_flat.wav")
    classic_bell = os.path.join(d, "classic_bell.wav")
    broad_bell = os.path.join(d, "broad_bell.wav")
    make_wav(tones_in, tones=[(100, 0.03), (1000, 0.03)])
    run(inp, broad_flat, f"{COMMON},SEQ_ON=1,SEQ_TYPE=1,SEQ_LF_F=300,SEQ_LF_G=0")
    run(tones_in, tones_flat,   f"{COMMON},SEQ_ON=1,SEQ_TYPE=0,SEQ_LMF_G=0")
    run(tones_in, classic_bell, f"{COMMON},SEQ_ON=1,SEQ_TYPE=0,SEQ_LMF_F=1000,SEQ_LMF_Q=1,SEQ_LMF_G=9")
    run(tones_in, broad_bell,   f"{COMMON},SEQ_ON=1,SEQ_TYPE=1,SEQ_LMF_F=1000,SEQ_LMF_Q=1,SEQ_LMF_G=9")
    BF, CB, BB = read_left(broad_flat), read_left(classic_bell), read_left(broad_bell)
    TF = read_left(tones_flat)
    d_type_flat = max(abs(b - dd) for b, dd in zip(BF, D))
    print(f"[ssl-it] flat Broad vs flat Classic max|d| = {d_type_flat:.3e}")
    assert d_type_flat < 1e-12, "flat Broad voicing is not transparent"
    c1k = 20 * math.log10(goertzel(CB, 1000) / goertzel(TF, 1000))
    b1k = 20 * math.log10(goertzel(BB, 1000) / goertzel(TF, 1000))
    c100 = 20 * math.log10(goertzel(CB, 100) / goertzel(TF, 100))
    b100 = 20 * math.log10(goertzel(BB, 100) / goertzel(TF, 100))
    print(f"[ssl-it] LMF +9 @1k: Classic {c1k:.2f}/{c100:.2f} dB, Broad {b1k:.2f}/{b100:.2f} dB (@1k/@100)")
    assert abs(c1k - 9.0) < 0.3 and abs(b1k - 9.0) < 0.3, "EQ types disagree on peak placement/gain"
    assert abs(c100 - 0.11) < 0.2 and abs(b100 - 0.87) < 0.2, "bell skirts off the model"

    print("ALL SSL INTEGRATION TESTS PASSED")


if __name__ == "__main__":
    main()
