# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "pandas", "pyarrow", "scipy"]
# ///
"""Fit the AMEK 9099-style EQ voicing from an eqds transfer-function dataset.

    uv run scripts/fit_amek_eq.py \
        --run /Volumes/External/Data/eq-dataset/runs/amek9099-v1-55884e47

Reads the run's manifest.parquet (plugin read-back knob values + the
precomputed `qa.tf_mag_db`, i.e. exactly what `eqds tf export` emits, joined to
the knobs) and fits a grey-box model of every section of the console EQ:

  HPF  3rd order (2nd-order HP + 1st-order HP), bilinear
  LF   shelf: general 2nd-order section (separate zero/pole Q), matched  |  bell: proportional-Q peak, matched
  LMF  RBJ peaking (bilinear), constant-Q with a small gain law
  HMF  RBJ peaking (bilinear), constant-Q with a small gain law
  HF   shelf: general 2nd-order section, matched                          |  bell: proportional-Q peak, matched
  LPF  two 2nd-order LP sections, matched (measures ~18 dB/oct + a pre-corner bump)

"matched" = poles by impulse invariance + numerator magnitude-matched to the
analog prototype (exact at DC and Nyquist, least squares in between), always
placing the lower/sharper pole pair and inverting when needed. Decramped: the
measured HF bands keep their gain up to Nyquist; the measured HMF bell is
bilinear-cramped to 0 dB at Nyquist, so the mid bells stay RBJ.

Knob convention of the shipped model (native/clap/src/amek_eq.hpp):
  freq  = where the section physically acts (bell centre, shelf pole/zero
          geometric centre, filter -3 dB point) — so flipping the EQ type keeps
          a band in place and changes only its shape;
  gain  = peak (bell) / asymptotic plateau (shelf) gain in dB;
  Q     = the console's own (broad) Q scale: Q_rbj ≈ κ·Q_knob, κ≈0.32.
The console's panel legends (non-uniform frequency labels, the S-shaped Q pot)
are NOT shipped; they are fitted here only as label→knob maps so the model can
be scored against the measured held-out settings.

Outputs (unless --no-write):
  native/clap/src/amek_eq_fit.hpp          generated coefficient tables
  native/clap/tests/amek_eq_golden.hpp     golden vectors: Python-model dB at
                                           fixed knob settings + measured eval TFs
and prints a validation report (per-example RMS/max dB error, 20 Hz–20 kHz).
"""

from __future__ import annotations

import argparse
import json
import math
import warnings
from pathlib import Path

import numpy as np
import pandas as pd
from scipy.optimize import least_squares

warnings.filterwarnings("ignore", category=RuntimeWarning)

REPO = Path(__file__).resolve().parents[1]
FS_FIT = 48000.0                      # dataset render rate
TF_F = np.geomspace(10.0, 24000.0, 256)   # eqds.dsp.tf_freq_grid()
SEL = (TF_F >= 20.0) & (TF_F <= 20000.0)  # scoring band
MAX_POLE_FRAC = 0.45                  # matched-design pole-frequency clamp (x fs)

# Reference frequencies for the shape laws' u = log2(f / fref), per section.
FREF = {"lf": 100.0, "lmf": 300.0, "hmf": 2000.0, "hf": 6000.0, "hpf": 100.0, "lpf": 8000.0}

# ---------------------------------------------------------------------------
# Digital section designs — the C++ (amek_eq.hpp) mirrors these exactly.
# Each returns (b0, b1, b2, a1, a2), a0-normalized.
# ---------------------------------------------------------------------------

def rbj_peak(g_db, f0, q, fs):
    """RBJ peaking EQ (== ssl_design type 0)."""
    f0 = min(max(f0, 1.0), 0.49 * fs); q = max(q, 1e-3)
    A = 10 ** (g_db / 40); w0 = 2 * math.pi * f0 / fs
    cw, sw = math.cos(w0), math.sin(w0); al = sw / (2 * q)
    a0 = 1 + al / A
    return ((1 + al * A) / a0, -2 * cw / a0, (1 - al * A) / a0, -2 * cw / a0, (1 - al / A) / a0)


def bilinear_hp2(fc, q, fs):
    """RBJ high-pass (bilinear, prewarped at fc) (== ssl_design type 3)."""
    fc = min(max(fc, 1.0), 0.49 * fs); q = max(q, 1e-3)
    w0 = 2 * math.pi * fc / fs; cw, sw = math.cos(w0), math.sin(w0); al = sw / (2 * q)
    a0 = 1 + al
    return ((1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0, -2 * cw / a0, (1 - al) / a0)


def bilinear_hp1(fc, fs):
    """First-order high-pass, bilinear prewarped at fc (embedded in a biquad)."""
    fc = min(max(fc, 1.0), 0.49 * fs)
    K = math.tan(math.pi * fc / fs)
    n = 1.0 / (1.0 + K)
    return (n, -n, 0.0, (K - 1.0) / (K + 1.0), 0.0)


def _analog_mag2(n2, n1, n0, d1, d0, w):
    """|H(jw)|^2 for H(s) = (n2 s^2 + n1 s + n0) / (s^2 + d1 s + d0)."""
    nr = n0 - n2 * w * w; ni = n1 * w
    dr = d0 - w * w;     di = d1 * w
    return (nr * nr + ni * ni) / max(dr * dr + di * di, 1e-300)


MATCH_POINTS = 16                     # numerator least-squares grid size


def _matched_core(n2, n1, n0, d1, d0, fs):
    """Impulse-invariant poles; numerator power coefficients B0/B1 pinned to the
    analog |H|^2 at DC/Nyquist, B2 by relative-error least squares over a log
    grid (closed form). |P(e^jw)|^2 = B0*phi0 + B1*phi1 + B2*phi2 with
    phi1 = sin^2(w/2), phi0 = 1 - phi1, phi2 = 4*phi0*phi1."""
    wn = math.sqrt(max(d0, 1e-30)); zeta = d1 / (2 * wn)
    wn = min(wn, 2 * math.pi * MAX_POLE_FRAC * fs)   # keep poles below Nyquist
    T = 1.0 / fs
    if zeta < 1.0:
        e = math.exp(-zeta * wn * T)
        a1 = -2 * e * math.cos(wn * T * math.sqrt(1 - zeta * zeta))
        a2 = e * e
    else:   # two real poles; slow root written cancellation-free
        sq = math.sqrt(zeta * zeta - 1)
        r_fast = math.exp(-wn * T * (zeta + sq))
        r_slow = math.exp(-wn * T / (zeta + sq))
        a1 = -(r_fast + r_slow)
        a2 = r_fast * r_slow
    A0 = (1 + a1 + a2) ** 2; A1 = (1 - a1 + a2) ** 2; A2 = -4 * a2
    B0 = _analog_mag2(n2, n1, n0, d1, d0, 0.0) * A0
    B1 = _analog_mag2(n2, n1, n0, d1, d0, math.pi * fs) * A1
    wz = math.sqrt(n0 / n2) if (n2 > 0 and n0 > 0) else wn
    f_lo = max(min(wn, wz) / (2 * math.pi) / 4, 5.0)
    f_hi = 0.499 * fs
    num = den = 0.0
    for k in range(MATCH_POINTS):
        f = f_lo * (f_hi / f_lo) ** (k / (MATCH_POINTS - 1))
        w = 2 * math.pi * f / fs
        p1 = math.sin(w / 2) ** 2; p0 = 1 - p1; p2 = 4 * p0 * p1
        tgt = _analog_mag2(n2, n1, n0, d1, d0, 2 * math.pi * f) * (A0 * p0 + A1 * p1 + A2 * p2)
        wt = 1.0 / max(tgt * tgt, 1e-300)
        num += wt * p2 * (tgt - B0 * p0 - B1 * p1)
        den += wt * p2 * p2
    B2 = num / den if den > 0 else 0.0
    sB0, sB1 = math.sqrt(max(B0, 0.0)), math.sqrt(max(B1, 0.0))
    W = 0.5 * (sB0 + sB1)
    disc = math.sqrt(max(W * W + B2, 0.0))
    return (0.5 * (W + disc), 0.5 * (sB0 - sB1), 0.5 * (W - disc), a1, a2)


def matched(n2, n1, n0, d1, d0, fs):
    """Decramped biquad for analog H(s) = (n2 s^2 + n1 s + n0)/(s^2 + d1 s + d0)
    (rad/s). The pole pair that impulse invariance must place is always the
    lower / sharper of H's two pairs: when the zeros are lower (a cut peak, a
    rising HF shelf, a falling LF shelf) we design 1/H and invert the result,
    which is exact because every section here is minimum phase."""
    if n2 > 0 and n0 > 0:
        wz = math.sqrt(n0 / n2); zz = n1 / (2 * n2 * wz)
        wp = math.sqrt(d0);      zp = d1 / (2 * wp)
        if wz < wp * (1 - 1e-9) or (abs(wz - wp) <= 1e-9 * wp and zz < zp):
            b0, b1, b2, a1, a2 = _matched_core(1 / n2, d1 / n2, d0 / n2, n1 / n2, n0 / n2, fs)
            return (1 / b0, a1 / b0, a2 / b0, b1 / b0, b2 / b0)
    return _matched_core(n2, n1, n0, d1, d0, fs)


# Analog prototypes (rad/s) ------------------------------------------------------

def proto_peak(g_db, f0, q):
    A = 10 ** (g_db / 40); w = 2 * math.pi * f0
    return (1.0, A * w / q, w * w, w / (A * q), w * w)


def proto_shelf(side, g_db, fg, qz, qp):
    """General 2nd-order shelf. LF: DC gain g, HF gain 0 dB. HF: the reverse.
    fz*fp = fg^2; (fz/fp)^(±2) = linear gain."""
    r = 10 ** (g_db / 80)
    if side == "lf":
        fz, fp = fg * r, fg / r
        k = 1.0
    else:
        fz, fp = fg / r, fg * r
        k = (fp / fz) ** 2
    wz, wp = 2 * math.pi * fz, 2 * math.pi * fp
    return (k, k * wz / qz, k * wz * wz, wp / qp, wp * wp)


def proto_lp(fc, q):
    w = 2 * math.pi * fc
    return (0.0, 0.0, w * w, w / q, w * w)


def mag_db(sections, f, fs):
    z = np.exp(-1j * 2 * np.pi * np.asarray(f) / fs)
    h = np.ones_like(z)
    for b0, b1, b2, a1, a2 in sections:
        h = h * (b0 + b1 * z + b2 * z * z) / (1 + a1 * z + a2 * z * z)
    return 20 * np.log10(np.maximum(np.abs(h), 1e-12))


# ---------------------------------------------------------------------------
# Shape laws: small polynomials in u = clamp(log2(f/fref)), v = clamp(g/18).
# ---------------------------------------------------------------------------

def feats(kind, u, v):
    if kind == "mid":     # multiplies log(q_knob) + log κ
        return [v, v * v, u, u * u]
    if kind == "bell":    # proportional-Q law of the LF/HF bells
        return [1.0, v, v * v, abs(v), u, u * u]
    if kind == "shelf":
        return [1.0, v, v * v, u, u * u, u * v]
    if kind == "shelf_d":  # log(Qp/Qz): vanishes at 0 dB so a flat shelf is exactly identity
        return [v, v * v, u * v]
    if kind == "filt":
        return [1.0, u, u * u]
    raise KeyError(kind)


def uv(band, f, g, urange):
    u = math.log2(max(f, 1e-3) / FREF[band])
    u = min(max(u, urange[0]), urange[1])
    v = min(max(g / 18.0, -1.0), 1.0)
    return u, v


class Model:
    """Knob -> digital sections, parameterized by the fitted tables."""

    def __init__(self, P):
        self.P = P   # dict of coefficient arrays + ranges

    def lmf_hmf(self, band, g, f, q, fs):
        c = self.P[band]; u, v = uv(band, f, g, c["urange"])
        lq = math.log(max(q, 1e-3)) + c["logk"] + float(np.dot(c["beta"], feats("mid", u, v)))
        return [rbj_peak(g, f, math.exp(lq), fs)]

    def bell(self, band, g, f, fs):
        c = self.P[band + "_bell"]; u, v = uv(band, f, g, c["urange"])
        q = math.exp(float(np.dot(c["beta"], feats("bell", u, v))))
        return [matched(*proto_peak(g, f, q), fs)]

    def shelf(self, band, g, f, fs):
        c = self.P[band + "_shelf"]; u, v = uv(band, f, g, c["urange"])
        qz = math.exp(float(np.dot(c["bz"], feats("shelf", u, v))))
        qp = qz * math.exp(float(np.dot(c["bd"], feats("shelf_d", u, v))))
        return [matched(*proto_shelf(band, g, f, qz, qp), fs)]

    def hpf(self, f3, fs):
        c = self.P["hpf"]; u, _ = uv("hpf", f3, 0.0, c["urange"]); ft = feats("filt", u, 0.0)
        fc2 = f3 * math.exp(float(np.dot(c["b_fc2"], ft)))
        q = math.exp(float(np.dot(c["b_q"], ft)))
        fc1 = fc2 * math.exp(float(np.dot(c["b_k"], ft)))
        return [bilinear_hp2(fc2, q, fs), bilinear_hp1(fc1, fs)]

    def lpf(self, f3, fs):
        c = self.P["lpf"]; u, _ = uv("lpf", f3, 0.0, c["urange"]); ft = feats("filt", u, 0.0)
        fa = f3 * math.exp(float(np.dot(c["b_fa"], ft)))
        qa = math.exp(float(np.dot(c["b_qa"], ft)))
        fb = fa * math.exp(float(np.dot(c["b_k"], ft)))
        qb = math.exp(float(np.dot(c["b_qb"], ft)))
        return [matched(*proto_lp(fa, qa), fs), matched(*proto_lp(fb, qb), fs)]

    def cascade(self, k, fs):
        """k: knob dict (Axon semantics). Returns the section list."""
        s = []
        if k.get("hpf_on"): s += self.hpf(k["hpf_f"], fs)
        if k.get("eq_on", True):
            if k.get("lf_g", 0.0) != 0.0:
                s += self.bell("lf", k["lf_g"], k["lf_f"], fs) if k.get("lf_bell") else self.shelf("lf", k["lf_g"], k["lf_f"], fs)
            if k.get("lmf_g", 0.0) != 0.0: s += self.lmf_hmf("lmf", k["lmf_g"], k["lmf_f"], k["lmf_q"], fs)
            if k.get("hmf_g", 0.0) != 0.0: s += self.lmf_hmf("hmf", k["hmf_g"], k["hmf_f"], k["hmf_q"], fs)
            if k.get("hf_g", 0.0) != 0.0:
                s += self.bell("hf", k["hf_g"], k["hf_f"], fs) if k.get("hf_bell") else self.shelf("hf", k["hf_g"], k["hf_f"], fs)
        if k.get("lpf_on"): s += self.lpf(k["lpf_f"], fs)
        return s


# ---------------------------------------------------------------------------
# Dataset access
# ---------------------------------------------------------------------------

COLS = ["example_id", "category", "split", "param.eq_on_off",
        "param.high_gain_db", "param.high_peak", "param.high_mid_gain_db", "param.high_mid_q",
        "param.high_mid_frequency_x2", "param.low_mid_gain_db", "param.low_mid_q",
        "param.low_mid_frequency_2", "param.low_gain_db", "param.low_peak",
        "param.highpass_filter_in", "param.highpass_filter_x3",
        "param.lowpass_filter_in", "param.lowpass_filter_3",
        "effective.hf_freq_hz", "effective.hmf_freq_hz", "effective.lmf_freq_hz",
        "effective.lf_freq_hz", "effective.hpf_freq_hz", "effective.lpf_freq_hz",
        "active.hf", "active.hmf", "active.lmf", "active.lf", "qa.tf_mag_db"]


def load_run(run: Path):
    m = pd.read_parquet(run / "manifest.parquet", columns=COLS)
    tf = np.stack([np.asarray(v, float) for v in m["qa.tf_mag_db"]])
    m = m.drop(columns=["qa.tf_mag_db"]).reset_index(drop=True)
    m["hp"] = m["param.highpass_filter_in"] == "In"
    m["lp"] = m["param.lowpass_filter_in"] == "In"
    m["nact"] = m[["active.hf", "active.hmf", "active.lmf", "active.lf"]].sum(axis=1)
    eq_off_flat = (~m["param.eq_on_off"]) & (~m.hp) & (~m.lp)
    offset = float(np.median(tf[eq_off_flat][:, SEL])) if eq_off_flat.any() else 0.0
    return m, tf - offset, offset


# ---------------------------------------------------------------------------
# Stage A: per-example fits of each section family (knob = nuisance params)
# ---------------------------------------------------------------------------

def _fit(res, starts, lo=None, hi=None):
    best = None
    for p0 in starts:
        kw = {} if lo is None else {"bounds": (lo, hi)}
        r = least_squares(res, p0, **kw)
        if best is None or r.cost < best.cost:
            best = r
    return best


def f3_measured(y, side):
    """-3 dB point of a measured filter TF (log-interpolated)."""
    lf = np.log(TF_F)
    if side == "hp":
        idx = np.where((y[:-1] < -3) & (y[1:] >= -3))[0]
        if not len(idx): return None
        i = idx[-1]
    else:
        idx = np.where((y[:-1] >= -3) & (y[1:] < -3))[0]
        if not len(idx): return None
        i = idx[0]
    t = (-3 - y[i]) / (y[i + 1] - y[i])
    return float(np.exp(lf[i] + t * (lf[i + 1] - lf[i])))


def stage_a(m, tf):
    rows = []
    single = (m.nact == 1) & ~m.hp & ~m.lp & m["param.eq_on_off"] & (m.split == "train")
    spec = {
        "lf":  ("param.low_gain_db", "effective.lf_freq_hz", "param.low_peak", None, None),
        "lmf": ("param.low_mid_gain_db", "effective.lmf_freq_hz", None, "param.low_mid_q", "param.low_mid_frequency_2"),
        "hmf": ("param.high_mid_gain_db", "effective.hmf_freq_hz", None, "param.high_mid_q", "param.high_mid_frequency_x2"),
        "hf":  ("param.high_gain_db", "effective.hf_freq_hz", "param.high_peak", None, None),
    }
    for band, (gc, fc, pc, qc, mc) in spec.items():
        for i in np.where(single & m["active." + band])[0]:
            g, f, y = float(m[gc].iat[i]), float(m[fc].iat[i]), tf[i]
            if qc:   # constant-Q RBJ bell (bilinear)
                q = float(m[qc].iat[i])
                def res(p):
                    return (mag_db([rbj_peak(p[0], f * math.exp(p[1]), math.exp(p[2]), FS_FIT)], TF_F, FS_FIT) - y)[SEL]
                r = _fit(res, [[g, 0.0, math.log(0.32 * q)], [g, 0.2, math.log(0.5)]])
                rows.append(dict(sec=band, i=i, g_lab=g, f_lab=f, q_lab=q,
                                 mult=bool(m[mc].iat[i]), G=r.x[0], F=f * math.exp(r.x[1]),
                                 Q=math.exp(r.x[2]), rms=float(np.sqrt(np.mean(r.fun ** 2)))))
            elif bool(m[pc].iat[i]):   # proportional-Q bell (matched)
                def res(p):
                    return (mag_db([matched(*proto_peak(p[0], f * math.exp(p[1]), math.exp(p[2])), FS_FIT)], TF_F, FS_FIT) - y)[SEL]
                r = _fit(res, [[g, 0.0, math.log(0.45)], [g, 0.2, math.log(0.7)]])
                rows.append(dict(sec=band + "_bell", i=i, g_lab=g, f_lab=f, G=r.x[0], F=f * math.exp(r.x[1]),
                                 Q=math.exp(r.x[2]), rms=float(np.sqrt(np.mean(r.fun ** 2)))))
            else:   # general 2nd-order shelf (matched)
                def res(p):
                    fg = f * math.exp(p[1])
                    return (mag_db([matched(*proto_shelf(band, p[0], fg, math.exp(p[2]), math.exp(p[3])), FS_FIT)], TF_F, FS_FIT) - y)[SEL]
                f0 = [0.0, -0.9] if band == "hf" else [0.0, 0.7]
                r = _fit(res, [[g, a, math.log(0.7), math.log(0.7)] for a in f0])
                rows.append(dict(sec=band + "_shelf", i=i, g_lab=g, f_lab=f, G=r.x[0], F=f * math.exp(r.x[1]),
                                 Qz=math.exp(r.x[2]), Qp=math.exp(r.x[3]), rms=float(np.sqrt(np.mean(r.fun ** 2)))))
    # filters alone
    for side, onc, other, fc, mc in [("hp", "hp", "lp", "effective.hpf_freq_hz", "param.highpass_filter_x3"),
                                     ("lp", "lp", "hp", "effective.lpf_freq_hz", "param.lowpass_filter_3")]:
        for i in np.where((m.nact == 0) & m[onc] & ~m[other] & (m.split == "train"))[0]:
            f, y = float(m[fc].iat[i]), tf[i]
            f3 = f3_measured(y, side)
            if f3 is None or f3 > 0.4 * FS_FIT:
                continue
            w = SEL & (y > -50)
            if side == "hp":
                def res(p):
                    fc2 = f3 * math.exp(p[0])
                    s = [bilinear_hp2(fc2, math.exp(p[1]), FS_FIT), bilinear_hp1(fc2 * math.exp(p[2]), FS_FIT)]
                    return (mag_db(s, TF_F, FS_FIT) - y)[w]
                r = _fit(res, [[0.0, 0.0, 0.0]])
                rows.append(dict(sec="hpf", i=i, f_lab=f, mult=bool(m[mc].iat[i]), F=f3,
                                 p=r.x.tolist(), rms=float(np.sqrt(np.mean(r.fun ** 2)))))
            else:
                def res(p):
                    fa = f3 * math.exp(p[0]); fb = fa * math.exp(p[2])
                    s = [matched(*proto_lp(fa, math.exp(p[1])), FS_FIT),
                         matched(*proto_lp(fb, math.exp(p[3])), FS_FIT)]
                    return (mag_db(s, TF_F, FS_FIT) - y)[w]
                r = _fit(res, [[0.0, math.log(0.55), 0.0, math.log(1.3)], [0.1, math.log(0.8), -0.3, math.log(0.6)]])
                rows.append(dict(sec="lpf", i=i, f_lab=f, mult=bool(m[mc].iat[i]), F=f3,
                                 p=r.x.tolist(), rms=float(np.sqrt(np.mean(r.fun ** 2)))))
    return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# Stage B: smooth shape laws (regression on stage-A params), then a joint
# refinement of the laws directly against the measured curves.
# ---------------------------------------------------------------------------

Q_KNOTS = np.array([0.65, 0.8, 0.95, 1.1, 1.25, 1.4, 1.6, 1.8, 2.0])


def hat_basis(x, knots):
    """Piecewise-linear (hat) basis, clamped at the ends."""
    x = np.clip(np.asarray(x, float), knots[0], knots[-1])
    B = np.zeros((len(x), len(knots)))
    j = np.clip(np.searchsorted(knots, x, side="right") - 1, 0, len(knots) - 2)
    t = (x - knots[j]) / (knots[j + 1] - knots[j])
    B[np.arange(len(x)), j] = 1 - t
    B[np.arange(len(x)), j + 1] = t
    return B


def urange_of(sec_rows, band, pad=0.0):
    u = np.log2(sec_rows.F.values / FREF[band])
    return [float(u.min() - pad), float(u.max() + pad)]


def fit_laws(A, m, tf):
    P = {}
    # --- LMF/HMF: log Q = log S(q_label) + beta·[v, v², u, u²];  κ from S
    for band in ("lmf", "hmf"):
        d = A[A.sec == band]
        ur = urange_of(d, band)
        U = np.array([uv(band, F, G, ur) for F, G in zip(d.F, d.G)])
        X = np.hstack([hat_basis(d.q_lab.values, Q_KNOTS),
                       np.array([feats("mid", u, v) for u, v in U])])
        coef, *_ = np.linalg.lstsq(X, np.log(d.Q.values), rcond=None)
        logS = coef[:len(Q_KNOTS)]; beta = coef[len(Q_KNOTS):]
        # κ: best proportional fit of S(q) over the panel range
        qg = np.linspace(Q_KNOTS[0], Q_KNOTS[-1], 200)
        logk = float(np.mean(hat_basis(qg, Q_KNOTS) @ logS - np.log(qg)))
        P[band] = dict(urange=ur, logk=logk, beta=beta.tolist(), logS=logS.tolist())
    # --- LF/HF bells: log Q = poly(v, v², |v|, u, u²)
    for band in ("lf", "hf"):
        d = A[A.sec == band + "_bell"]
        ur = urange_of(d, band)
        X = np.array([feats("bell", *uv(band, F, G, ur)) for F, G in zip(d.F, d.G)])
        beta, *_ = np.linalg.lstsq(X, np.log(d.Q.values), rcond=None)
        P[band + "_bell"] = dict(urange=ur, beta=beta.tolist())
    # --- shelves: log Qz, log Qp = poly(...), weighted by |G| (tiny gains carry no shape)
    for band in ("lf", "hf"):
        d = A[A.sec == band + "_shelf"]
        ur = urange_of(d, band)
        UV = [uv(band, F, G, ur) for F, G in zip(d.F, d.G)]
        X = np.array([feats("shelf", u, v) for u, v in UV])
        Xd = np.array([feats("shelf_d", u, v) for u, v in UV])
        w = np.clip(np.abs(d.G.values) / 6.0, 0.05, 1.0)[:, None]
        bz, *_ = np.linalg.lstsq(X * w, np.log(d.Qz.values) * w[:, 0], rcond=None)
        bd, *_ = np.linalg.lstsq(Xd * w, np.log(d.Qp.values / d.Qz.values) * w[:, 0], rcond=None)
        P[band + "_shelf"] = dict(urange=ur, bz=bz.tolist(), bd=bd.tolist())
    # --- filters
    for sec, names in (("hpf", ("b_fc2", "b_q", "b_k")), ("lpf", ("b_fa", "b_qa", "b_k", "b_qb"))):
        d = A[A.sec == sec]
        ur = urange_of(d, sec)
        X = np.array([feats("filt", uv(sec, F, 0.0, ur)[0], 0.0) for F in d.F])
        Pp = np.array(d.p.tolist())
        c = {"urange": ur}
        for j, nm in enumerate(names):
            c[nm] = np.linalg.lstsq(X, Pp[:, j], rcond=None)[0].tolist()
        P[sec] = c
    return P


def refine(P, A, tf):
    """Joint refinement: optimise each section's shape law directly against the
    measured curves, with per-example knob values (freq, gain) as nuisance."""
    model = Model(P)

    def sections_for(sec, knob):
        if sec in ("lmf", "hmf"): return model.lmf_hmf(sec, knob["G"], knob["F"], knob["q"], FS_FIT)
        if sec.endswith("_bell"): return model.bell(sec[:-5], knob["G"], knob["F"], FS_FIT)
        if sec.endswith("_shelf"): return model.shelf(sec[:-6], knob["G"], knob["F"], FS_FIT)
        if sec == "hpf": return model.hpf(knob["F"], FS_FIT)
        if sec == "lpf": return model.lpf(knob["F"], FS_FIT)

    layout = {"lmf": ["beta", "logS"], "hmf": ["beta", "logS"],
              "lf_bell": ["beta"], "hf_bell": ["beta"],
              "lf_shelf": ["bz", "bd"], "hf_shelf": ["bz", "bd"],
              "hpf": ["b_fc2", "b_q", "b_k"], "lpf": ["b_fa", "b_qa", "b_k", "b_qb"]}
    for sec, keys in layout.items():
        d = A[A.sec == sec].reset_index(drop=True)
        c = P[sec]
        sizes = [len(c[k]) for k in keys]
        # Filter knobs are pinned to the measured -3 dB point (the knob
        # convention; a free cutoff would trade off against the law's own
        # frequency offset). Band knobs (freq, gain) are free nuisance.
        is_filter = sec in ("hpf", "lpf")
        n_nuis = 0 if is_filter else 2
        g0 = np.concatenate([np.asarray(c[k], float) for k in keys])
        nu0 = (np.zeros(0) if is_filter else
               np.array([[math.log(F), G] for F, G in zip(d.F, d.G)]).ravel())

        def unpack(x):
            off = 0
            for k, n in zip(keys, sizes):
                c[k] = x[off:off + n].tolist(); off += n
            return x[off:].reshape(len(d), n_nuis)

        def res(x):
            nu = unpack(x)
            out = []
            for j in range(len(d)):
                y = tf[int(d.i[j])]
                if is_filter:
                    knob = {"F": float(d.F[j])}
                else:
                    knob = {"F": math.exp(nu[j, 0]), "G": nu[j, 1]}
                if sec in ("lmf", "hmf"):
                    # mid bells: q_knob = S(q_label) / κ  (label map fitted jointly)
                    logS = hat_basis([d.q_lab[j]], Q_KNOTS)[0] @ np.asarray(c["logS"])
                    knob["q"] = math.exp(logS - c["logk"])
                w = SEL & (y > -50) if is_filter else SEL
                out.append((mag_db(sections_for(sec, knob), TF_F, FS_FIT) - y)[w] / math.sqrt(w.sum()))
            return np.concatenate(out)

        # Sparsity: the global law touches every residual; each example's
        # nuisance knobs touch only that example's residuals.
        lens = [int((SEL & (tf[int(d.i[j])] > -50)).sum()) if is_filter else int(SEL.sum())
                for j in range(len(d))]
        ng = len(g0)
        S = np.zeros((sum(lens), ng + len(d) * n_nuis), dtype=bool)
        S[:, :ng] = True
        row = 0
        for j, n in enumerate(lens):
            S[row:row + n, ng + j * n_nuis: ng + (j + 1) * n_nuis] = True
            row += n
        x0 = np.concatenate([g0, nu0])
        r = least_squares(res, x0, x_scale="jac", max_nfev=40, jac_sparsity=S)
        nu = unpack(r.x)
        if sec in ("lmf", "hmf"):   # re-derive κ from the refined S
            qg = np.linspace(Q_KNOTS[0], Q_KNOTS[-1], 200)
            c["logk"] = float(np.mean(hat_basis(qg, Q_KNOTS) @ np.asarray(c["logS"]) - np.log(qg)))
        if not is_filter:
            idx = A.index[A.sec == sec]
            A.loc[idx, "F"] = np.exp(nu[:, 0])
            A.loc[idx, "G"] = nu[:, 1]
        print(f"  refine {sec:9s} n={len(d):4d}  per-point rms {math.sqrt(2 * r.cost / len(d)):.3f} dB")
    return P, A


# ---------------------------------------------------------------------------
# Label -> knob maps (evaluation only; the plugin never sees panel labels)
# ---------------------------------------------------------------------------

def fit_label_maps(A, m):
    L = {}
    for sec in A.sec.unique():
        d = A[A.sec == sec]
        band = sec.split("_")[0]
        mult = {"lmf": 0.5, "hmf": 2.0, "hpf": 3.0, "lpf": 1 / 3}.get(band)
        dial = (d.f_lab.values / np.where(d["mult"].astype(bool).values, mult, 1.0)) if mult else d.f_lab.values
        kn = np.geomspace(dial.min(), dial.max(), 14)
        Xf = hat_basis(np.log(dial), np.log(kn))
        has_gain = sec not in ("hpf", "lpf")
        if has_gain:
            vl = d.g_lab.values / 18.0
            Xf = np.hstack([Xf, vl[:, None], (vl * vl)[:, None], (vl * np.log(dial))[:, None]])
        cf = np.linalg.lstsq(Xf, np.log(d.F.values / d.f_lab.values), rcond=None)[0]
        e = dict(fknots=np.log(kn).tolist(), cf=cf.tolist(), mult=mult)
        if has_gain:
            gk = np.linspace(-18, 18, 13)
            e["gknots"] = gk.tolist()
            e["cg"] = np.linalg.lstsq(hat_basis(d.g_lab.values, gk), d.G.values, rcond=None)[0].tolist()
        L[sec] = e
    return L


def label_to_knob(L, sec, f_lab, g_lab=0.0, mult_on=False):
    e = L[sec]
    dial = f_lab / (e["mult"] if (e["mult"] and mult_on) else 1.0)
    x = hat_basis([math.log(dial)], np.array(e["fknots"]))[0]
    if "cg" in e:
        vl = g_lab / 18.0
        x = np.append(x, [vl, vl * vl, vl * math.log(dial)])
    F = f_lab * math.exp(float(x @ np.asarray(e["cf"])))
    G = float(hat_basis([g_lab], np.array(e["gknots"]))[0] @ np.asarray(e["cg"])) if "cg" in e else 0.0
    return F, G


def example_knobs(m, i, P, L, use_labels=False):
    r = m.iloc[i]
    k = {"eq_on": bool(r["param.eq_on_off"]), "hpf_on": bool(r.hp), "lpf_on": bool(r.lp)}

    def fg(sec, f, g, mult_on=False):
        return (f, g) if use_labels else label_to_knob(L, sec, f, g, mult_on)

    if r.hp: k["hpf_f"] = fg("hpf", r["effective.hpf_freq_hz"], 0.0, bool(r["param.highpass_filter_x3"]))[0]
    if r.lp: k["lpf_f"] = fg("lpf", r["effective.lpf_freq_hz"], 0.0, bool(r["param.lowpass_filter_3"]))[0]
    for band, gc, fc, pc in [("lf", "param.low_gain_db", "effective.lf_freq_hz", "param.low_peak"),
                             ("hf", "param.high_gain_db", "effective.hf_freq_hz", "param.high_peak")]:
        if r["active." + band]:
            bell = bool(r[pc]); sec = band + ("_bell" if bell else "_shelf")
            k[band + "_f"], k[band + "_g"] = fg(sec, r[fc], r[gc])
            k[band + "_bell"] = bell
    for band, gc, fc, qc, mc in [("lmf", "param.low_mid_gain_db", "effective.lmf_freq_hz", "param.low_mid_q", "param.low_mid_frequency_2"),
                                 ("hmf", "param.high_mid_gain_db", "effective.hmf_freq_hz", "param.high_mid_q", "param.high_mid_frequency_x2")]:
        if r["active." + band]:
            k[band + "_f"], k[band + "_g"] = fg(band, r[fc], r[gc], bool(r[mc]))
            q = float(r[qc])
            if not use_labels:
                c = P[band]
                q = math.exp(float(hat_basis([q], Q_KNOTS)[0] @ np.asarray(c["logS"])) - c["logk"])
            k[band + "_q"] = q
    return k


# SSL voicing at the same knobs (the plugin's other EQ type), for reference.
def ssl_sections(k, fs):
    from math import sqrt
    def ssl(t, g, f, q):
        f = min(max(f, 1.0), 0.49 * fs); q = max(q, 1e-3)
        A = 10 ** (g / 40); w0 = 2 * math.pi * f / fs; cw, sw = math.cos(w0), math.sin(w0)
        al = sw / (2 * q); sA = sqrt(A)
        if t == 1:
            b = (A * ((A + 1) - (A - 1) * cw + 2 * sA * al), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - 2 * sA * al))
            a = ((A + 1) + (A - 1) * cw + 2 * sA * al, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - 2 * sA * al)
        elif t == 2:
            b = (A * ((A + 1) + (A - 1) * cw + 2 * sA * al), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - 2 * sA * al))
            a = ((A + 1) - (A - 1) * cw + 2 * sA * al, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - 2 * sA * al)
        elif t == 3:
            b = ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2); a = (1 + al, -2 * cw, 1 - al)
        elif t == 4:
            b = ((1 - cw) / 2, 1 - cw, (1 - cw) / 2); a = (1 + al, -2 * cw, 1 - al)
        else:
            b = (1 + al * A, -2 * cw, 1 - al * A); a = (1 + al / A, -2 * cw, 1 - al / A)
        return (b[0] / a[0], b[1] / a[0], b[2] / a[0], a[1] / a[0], a[2] / a[0])
    s = []
    if k.get("hpf_on"): s += [ssl(3, 0, k["hpf_f"], 0.7071)] * 2
    if k.get("eq_on", True):
        if "lf_g" in k: s.append(ssl(0 if k["lf_bell"] else 1, k["lf_g"], k["lf_f"], 0.7071))
        if "lmf_g" in k: s.append(ssl(0, k["lmf_g"], k["lmf_f"], k["lmf_q"]))
        if "hmf_g" in k: s.append(ssl(0, k["hmf_g"], k["hmf_f"], k["hmf_q"]))
        if "hf_g" in k: s.append(ssl(0 if k["hf_bell"] else 2, k["hf_g"], k["hf_f"], 0.7071))
    if k.get("lpf_on"): s.append(ssl(4, 0, k["lpf_f"], 0.7071))
    return s


def evaluate(m, tf, P, L, split):
    model = Model(P)
    out = []
    for i in np.where(m.split == split)[0]:
        k = example_knobs(m, i, P, L)
        y = tf[i]
        w = SEL & (y > -40)          # ignore deep filter stop-bands (noise floor)
        e_amek = mag_db(model.cascade(k, FS_FIT), TF_F, FS_FIT) - y
        e_ssl = mag_db(ssl_sections(k, FS_FIT), TF_F, FS_FIT) - y
        k_lab = example_knobs(m, i, P, L, use_labels=True)
        e_lab = mag_db(model.cascade(k_lab, FS_FIT), TF_F, FS_FIT) - y
        out.append(dict(i=i, nact=int(m.nact.iat[i]), hp=bool(m.hp.iat[i]), lp=bool(m.lp.iat[i]),
                        rms=float(np.sqrt(np.mean(e_amek[w] ** 2))), mx=float(np.abs(e_amek[w]).max()),
                        rms_ssl=float(np.sqrt(np.mean(e_ssl[w] ** 2))),
                        rms_lab=float(np.sqrt(np.mean(e_lab[w] ** 2)))))
    return pd.DataFrame(out)


# ---------------------------------------------------------------------------
# C++ emission
# ---------------------------------------------------------------------------

def cpp_arr(name, xs):
    return f"inline constexpr double {name}[{len(xs)}] = {{{', '.join(f'{x:.12g}' for x in xs)}}};"


def emit_fit_header(P, path: Path, run_id: str, report: str):
    L = ["// GENERATED by scripts/fit_amek_eq.py — do not edit by hand.",
         f"// Source dataset: eqds run {run_id} (qa.tf_mag_db + plugin read-back knobs).",
         "// Shape-law coefficients for the AMEK 9099-style EQ voicing (amek_eq.hpp).",
         "//", *[f"// {ln}" for ln in report.splitlines()], "",
         "#pragma once", "", "namespace nablafx::amek_fit {", ""]
    for band in ("lf", "lmf", "hmf", "hf", "hpf", "lpf"):
        L.append(f"inline constexpr double k{band.upper()}_FREF = {FREF[band]:.1f};")
    L.append("")
    for band in ("lmf", "hmf"):
        c = P[band]; B = band.upper()
        L += [cpp_arr(f"k{B}_URANGE", c["urange"]), f"inline constexpr double k{B}_LOGK = {c['logk']:.12g};",
              cpp_arr(f"k{B}_BETA", c["beta"]), ""]
    for band in ("lf", "hf"):
        B = band.upper()
        c = P[band + "_bell"]
        L += [cpp_arr(f"k{B}_BELL_URANGE", c["urange"]), cpp_arr(f"k{B}_BELL_BETA", c["beta"])]
        c = P[band + "_shelf"]
        L += [cpp_arr(f"k{B}_SHELF_URANGE", c["urange"]), cpp_arr(f"k{B}_SHELF_BZ", c["bz"]),
              cpp_arr(f"k{B}_SHELF_BD", c["bd"]), ""]
    c = P["hpf"]
    L += [cpp_arr("kHPF_URANGE", c["urange"]), cpp_arr("kHPF_B_FC2", c["b_fc2"]),
          cpp_arr("kHPF_B_Q", c["b_q"]), cpp_arr("kHPF_B_K", c["b_k"]), ""]
    c = P["lpf"]
    L += [cpp_arr("kLPF_URANGE", c["urange"]), cpp_arr("kLPF_B_FA", c["b_fa"]),
          cpp_arr("kLPF_B_QA", c["b_qa"]), cpp_arr("kLPF_B_K", c["b_k"]), cpp_arr("kLPF_B_QB", c["b_qb"]), ""]
    L += ["}  // namespace nablafx::amek_fit", ""]
    path.write_text("\n".join(L))


GOLDEN_KNOBS = [
    dict(lmf_g=9.0, lmf_f=400.0, lmf_q=1.0),
    dict(hmf_g=-12.0, hmf_f=3000.0, hmf_q=1.6),
    dict(hmf_g=15.0, hmf_f=8000.0, hmf_q=0.7),
    dict(lf_g=-12.0, lf_f=120.0, lf_bell=False),
    dict(lf_g=8.0, lf_f=60.0, lf_bell=True),
    dict(hf_g=10.0, hf_f=4000.0, hf_bell=False),
    dict(hf_g=-9.0, hf_f=12000.0, hf_bell=True),
    dict(hpf_on=True, hpf_f=80.0),
    dict(lpf_on=True, lpf_f=9000.0),
    dict(hpf_on=True, hpf_f=40.0, lf_g=4.0, lf_f=90.0, lmf_g=-3.0, lmf_f=300.0, lmf_q=1.4,
         hmf_g=5.0, hmf_f=2500.0, hmf_q=0.9, hf_g=6.0, hf_f=10000.0, lpf_on=True, lpf_f=18000.0),
]
GOLDEN_F = [30.0, 60.0, 120.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0, 16000.0, 19000.0]


def emit_golden(P, m, tf, L, path: Path, run_id: str, eval_ids):
    model = Model(P)
    lines = ["// GENERATED by scripts/fit_amek_eq.py — do not edit by hand.",
             f"// Golden vectors for test_amek_eq (eqds run {run_id}).", "#pragma once", "",
             "#include <array>", "", "namespace amek_golden {", "",
             "struct Knobs { bool hpf_on; double hpf_f; bool lpf_on; double lpf_f;",
             "               double lf_g, lf_f; bool lf_bell; double lmf_g, lmf_f, lmf_q;",
             "               double hmf_g, hmf_f, hmf_q; double hf_g, hf_f; bool hf_bell; };", ""]

    def knob_lit(k):
        g = lambda n, d: k.get(n, d)
        b = lambda n: "true" if k.get(n) else "false"
        return (f"{{{b('hpf_on')}, {g('hpf_f', 80.0)}, {b('lpf_on')}, {g('lpf_f', 20000.0)}, "
                f"{g('lf_g', 0.0)}, {g('lf_f', 100.0)}, {b('lf_bell')}, {g('lmf_g', 0.0)}, {g('lmf_f', 500.0)}, {g('lmf_q', 1.0)}, "
                f"{g('hmf_g', 0.0)}, {g('hmf_f', 3000.0)}, {g('hmf_q', 1.0)}, {g('hf_g', 0.0)}, {g('hf_f', 10000.0)}, {b('hf_bell')}}}")

    lines.append(f"inline constexpr std::array<double, {len(GOLDEN_F)}> kFreqs = {{{', '.join(str(f) for f in GOLDEN_F)}}};")
    lines.append("struct Model { Knobs k; double fs; std::array<double, %d> db; };" % len(GOLDEN_F))
    rows = []
    for fs in (48000.0, 44100.0, 96000.0):
        for k in GOLDEN_KNOBS:
            db = mag_db(model.cascade(k, fs), GOLDEN_F, fs)
            rows.append(f"    {{{knob_lit(k)}, {fs}, {{{', '.join(f'{v:.6f}' for v in db)}}}}},")
    lines += [f"inline const Model kModel[{len(rows)}] = {{", *rows, "};", ""]
    # measured held-out TFs (dataset label -> knob via the fitted panel maps)
    Fm = TF_F[SEL][::4]
    lines.append(f"inline constexpr std::array<double, {len(Fm)}> kMeasFreqs = {{{', '.join(f'{f:.4f}' for f in Fm)}}};")
    lines.append("struct Measured { const char* id; Knobs k; bool eq_on; std::array<double, %d> db; };" % len(Fm))
    rows = []
    for i in eval_ids:
        k = example_knobs(m, i, P, L)
        y = np.interp(Fm, TF_F, tf[i])
        rows.append(f"    {{\"{m.example_id.iat[i]}\", {knob_lit(k)}, {'true' if k['eq_on'] else 'false'}, "
                    f"{{{', '.join(f'{v:.4f}' for v in y)}}}}},")
    lines += [f"inline const Measured kMeasured[{len(rows)}] = {{", *rows, "};", "", "}  // namespace amek_golden", ""]
    path.write_text("\n".join(lines))


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", type=Path, required=True, help="eqds run dir (manifest.parquet)")
    ap.add_argument("--no-write", action="store_true", help="report only; do not regenerate the C++ headers")
    ap.add_argument("--dump", type=Path, help="write fitted tables + eval metrics JSON here")
    args = ap.parse_args()

    run_id = args.run.name
    m, tf, offset = load_run(args.run)
    print(f"{run_id}: {len(m)} examples; removed flat console offset {offset:+.4f} dB")

    A = stage_a(m, tf)
    print("stage A (per-example free fits, train split):")
    for sec, d in A.groupby("sec"):
        print(f"  {sec:9s} n={len(d):4d}  rms median {d.rms.median():.3f}  p90 {d.rms.quantile(.9):.3f} dB")
    P = fit_laws(A, m, tf)
    print("joint refinement:")
    P, A = refine(P, A, tf)
    L = fit_label_maps(A, m)

    rep = []
    for split in ("train", "eval"):
        E = evaluate(m, tf, P, L, split)
        def q(s): return f"median {s.median():.3f} / p90 {s.quantile(.9):.3f} / max {s.max():.3f}"
        rep.append(f"{split:5s} n={len(E)}  AMEK model rms dB: {q(E.rms)}")
        rep.append(f"{'':5s}           AMEK model max|err| dB: {q(E.mx)}")
        rep.append(f"{'':5s}           panel labels as knobs rms: {q(E.rms_lab)}")
        rep.append(f"{'':5s}           SSL voicing, same knobs rms: {q(E.rms_ssl)}")
        if split == "eval":
            Ee = E
    print("\nvalidation (20 Hz–20 kHz, stop-bands below -40 dB ignored):")
    report = "\n".join(rep)
    print(report)
    print("\neval error by active-band count:")
    print(Ee.groupby("nact").rms.describe(percentiles=[.5, .9])[["count", "50%", "90%", "max"]].round(3).to_string())
    for band in ("lmf", "hmf"):
        print(f"{band}: kappa = {math.exp(P[band]['logk']):.4f} (Q_rbj = kappa * Q_knob at 0 dB, mid-range)")

    if args.dump:
        args.dump.write_text(json.dumps(dict(P=P, L=L, eval=Ee.to_dict("list")), indent=1))
    if not args.no_write:
        src = REPO / "native/clap/src/amek_eq_fit.hpp"
        emit_fit_header(P, src, run_id, "Validation vs measured curves (per-example RMS over 20 Hz-20 kHz):\n" + report)
        # a spread of held-out examples: worst, median-ish and multi-band/filter cases
        Es = Ee.sort_values("rms")
        pick = list(Es.i.iloc[np.linspace(0, len(Es) - 1, 16).astype(int)])
        emit_golden(P, m, tf, L, REPO / "native/clap/tests/amek_eq_golden.hpp", run_id, pick)
        print(f"\nwrote {src.relative_to(REPO)} and native/clap/tests/amek_eq_golden.hpp")


if __name__ == "__main__":
    main()
