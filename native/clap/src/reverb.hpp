// Transparent mastering room reverb — a tasteful, colourless sense of SPACE,
// WIDTH and DEPTH that does NOT touch clarity or perceived volume. The brief:
// "a little goes a long way." Every design choice below exists to add space
// without smearing transients, muddying the low end, or shifting loudness.
//
//   input ──┬──────────────────────────────────────────────────► (+) ── out
//           │  (dry: UNTOUCHED, unity, ZERO latency)                ▲
//           └─► [send HPF] ─► [pre-delay] ─► 8-line FDN ─► width ─► mix ┘
//                (low cut)      (wet only)    (Householder)   (decorr)
//
// Why each piece (clarity- & volume-first):
//   * FDN, not an IR/convolution: an 8-line Feedback Delay Network with a
//     LOSSLESS mixing matrix produces the smoothest, most colourless ("dense,
//     no flutter, no metallic ring") tail of any cheap reverb, with no IR file
//     to ship and no fixed colour. That transparency is the whole point.
//   * SEND HIGH-PASS (Low Cut): the signal feeding the reverb is high-passed so
//     the BASS IS NEVER REVERBERATED. This is the single most important
//     mastering move — it keeps the low end tight/centred and avoids the mud a
//     full-range reverb tail dumps under the mix. The dry low end is untouched.
//   * DAMPING (one-pole LPF in every feedback path): models air absorption —
//     HF decays faster than LF, so the tail darkens naturally and never smears
//     sibilance or transient "air" back over the music. Protects clarity.
//   * PRE-DELAY on the WET ONLY: a few ms of gap between the dry transient and
//     the onset of the tail buys DEPTH/separation and keeps the dry attack
//     legible. The dry is NEVER delayed, so the stage reports ZERO latency.
//   * WIDTH via DECORRELATION: L and R are built from DIFFERENT sign/tap
//     combinations of the 8 delay outputs, so the tail is genuinely stereo.
//     The Width control crossfades wet between mono (L=R, narrow) and the
//     decorrelated pair (wide). MONO-COMPATIBLE BY CONSTRUCTION: R is never a
//     phase-inverted copy of L, so L+R never collapses to silence.
//   * LOUDNESS-GENTLE MIX: out = dry + mix*wet, dry passed at UNITY. The wet is
//     roughly energy-normalised so a given Mix sounds consistent across Size /
//     Damp. At mix == 0 the output is BIT-IDENTICAL to the input (early return).
//
// Header-only, pure DSP in namespace nablafx, NO CLAP/ORT/std deps beyond the
// standard library — unit-testable standalone (cf. exciter.hpp, bass_mono.hpp).
//
//   Reverb rv;
//   rv.prepare(44100.0);
//   rv.set_params(/*mix*/0.2f, /*size*/0.3f, /*width*/0.8f,
//                 /*damp_hz*/7000.f, /*lowcut_hz*/250.f);
//   rv.process(L, R, n);     // in place, stereo
//   rv.reset();
//
// Latency: the wet pre-delay is musical (reverb is inherently late) and the dry
// path is not delayed at all, so this stage adds ZERO reported latency.

#pragma once
#include <array>
#include <cmath>
#include <vector>

namespace nablafx {

class Reverb {
public:
    static constexpr int kLines = 8;

    void prepare(double sample_rate) {
        sr_ = sample_rate > 0.0 ? sample_rate : 44100.0;

        // Size all buffers to the MAX they can ever need (RT safety: no alloc in
        // process()). Delay lengths scale up by kMaxSizeScale at Size=1; the
        // pre-delay maxes at kMaxPredelayMs. Round up generously.
        for (int i = 0; i < kLines; ++i) {
            const int base = kBaseLen[i];
            const int maxlen = static_cast<int>(std::ceil(base * kMaxSizeScale
                                * (sr_ / kDesignSR))) + 4;
            line_[i].buf.assign(static_cast<size_t>(maxlen), 0.0);
        }
        const int max_pd = static_cast<int>(std::ceil(kMaxPredelayMs * 1e-3 * sr_)) + 4;
        for (auto& pd : predelay_) pd.assign(static_cast<size_t>(max_pd), 0.0);

        reset();      // clear all state FIRST (it default-constructs the
        design_();    // per-line damping/feedback state), THEN design coeffs.
        coeffs_dirty_ = false;
    }

    void reset() {
        for (auto& ln : line_) {
            std::fill(ln.buf.begin(), ln.buf.end(), 0.0);
            ln.widx = 0;
            ln.damp_z = 0.0;
        }
        for (auto& pd : predelay_) std::fill(pd.begin(), pd.end(), 0.0);
        pd_idx_ = 0;
        send_hpf_[0].clear(); send_hpf_[1].clear();
    }

    // mix       : 0..1  parallel wet blend (0 = bit-identical bypass)
    // size      : 0..1  room size → RT60 + delay-length scaling
    // width     : 0..1  stereo width of the tail (0 = mono, 1 = fully decorrelated)
    // damp_hz   : 2000..18000  tail-damping one-pole LPF cutoff (air absorption)
    // lowcut_hz : 20..1000     reverb-send high-pass (bass is not reverberated)
    void set_params(float mix, float size, float width,
                    float damp_hz, float lowcut_hz) {
        mix_   = clamp01_(mix);
        width_ = clamp01_(width);
        const float sz = clamp01_(size);
        if (sz != size_ || damp_hz != damp_hz_ || lowcut_hz != lowcut_hz_
            || coeffs_dirty_) {
            size_      = sz;
            damp_hz_   = damp_hz;
            lowcut_hz_ = lowcut_hz;
            design_();
            coeffs_dirty_ = false;
        }
    }

    // In place, stereo. mix == 0 → input is returned bit-identical.
    void process(float* l, float* r, int n) {
        if (mix_ <= 0.f) return;
        const bool stereo = (r != nullptr);
        for (int i = 0; i < n; ++i) {
            const double dryL = static_cast<double>(l[i]);
            const double dryR = stereo ? static_cast<double>(r[i]) : dryL;

            // --- Reverb SEND --------------------------------------------------
            // Feed a mid-dominant sum (with a touch of side for early stereo
            // seeding) into the network, then high-pass it so the bass is never
            // reverberated. The dry stays full-range and untouched.
            const double mid  = 0.5 * (dryL + dryR);
            const double side = 0.5 * (dryL - dryR);
            const double sendL = send_hpf_[0].process(mid + 0.5 * side);
            const double sendR = stereo ? send_hpf_[1].process(mid - 0.5 * side)
                                        : sendL;

            // --- PRE-DELAY (wet only) ----------------------------------------
            predelay_[0][pd_idx_] = sendL;
            predelay_[1][pd_idx_] = sendR;
            const int rd = pd_idx_ - pd_len_;
            const int rdx = rd < 0 ? rd + static_cast<int>(predelay_[0].size()) : rd;
            const double inL = predelay_[0][static_cast<size_t>(rdx)];
            const double inR = predelay_[1][static_cast<size_t>(rdx)];
            pd_idx_ = (pd_idx_ + 1) % static_cast<int>(predelay_[0].size());

            // --- Read the 8 delay-line outputs -------------------------------
            std::array<double, kLines> out;
            for (int k = 0; k < kLines; ++k) {
                const int rp = line_[k].widx - line_[k].len;
                const int rpx = rp < 0 ? rp + static_cast<int>(line_[k].buf.size()) : rp;
                out[k] = line_[k].buf[static_cast<size_t>(rpx)];
            }

            // --- Lossless Householder mixing of the feedback -----------------
            // y = x - (2/N) * sum(x). Reflection: orthonormal (lossless), O(N),
            // and very smooth — scatters energy across all lines each pass.
            double sum = 0.0;
            for (int k = 0; k < kLines; ++k) sum += out[k];
            const double hf = (2.0 / kLines) * sum;

            // --- Write back: damping LPF in each feedback path, + input ------
            // Split the stereo send across the lines (even lines fed by L,
            // odd by R) so the network seeds a decorrelated field.
            for (int k = 0; k < kLines; ++k) {
                double fb = (out[k] - hf) * fb_gain_[k];
                // One-pole low-pass (air absorption). z += a*(x - z); y = z.
                line_[k].damp_z += damp_a_ * (fb - line_[k].damp_z);
                const double fed = line_[k].damp_z;
                const double inj = (k & 1) ? inR : inL;
                line_[k].buf[static_cast<size_t>(line_[k].widx)] = fed + inj * in_gain_;
                line_[k].widx = (line_[k].widx + 1)
                                % static_cast<int>(line_[k].buf.size());
            }

            // --- Build the decorrelated stereo wet ---------------------------
            // L and R use DIFFERENT signed tap combinations of the 8 outputs so
            // the two channels are decorrelated (= width). A mono reference (the
            // plain sum) is crossfaded against the wide pair by `width_`.
            double wL = 0.0, wR = 0.0, wMono = 0.0;
            for (int k = 0; k < kLines; ++k) {
                wL    += kTapL[k] * out[k];
                wR    += kTapR[k] * out[k];
                wMono += out[k];
            }
            wL *= kTapNorm; wR *= kTapNorm;
            wMono *= (1.0 / kLines);
            // width: 0 → both channels = mono sum (narrow); 1 → decorrelated.
            const double outWetL = (1.0 - width_) * wMono + width_ * wL;
            const double outWetR = (1.0 - width_) * wMono + width_ * wR;

            // --- Parallel blend: dry at unity + mix * normalised wet ---------
            l[i] = static_cast<float>(dryL + mix_ * wet_norm_ * outWetL);
            if (stereo)
                r[i] = static_cast<float>(dryR + mix_ * wet_norm_ * outWetR);
        }
    }

    // This stage adds ZERO reported latency (dry is never delayed).
    int latency_samples() const { return 0; }

private:
    // One-pole / biquad helpers ------------------------------------------------
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        double process(double x) {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void clear() { z1 = z2 = 0; }
        void set(double nb0, double nb1, double nb2, double na1, double na2) {
            b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
        }
    };

    struct Line {
        std::vector<double> buf;   // delay ring (sized in prepare to max length)
        int    len   = 0;          // current active delay length (<= buf.size())
        int    widx  = 0;          // write index
        double damp_z = 0.0;       // one-pole damping state in the feedback path
    };

    static double clamp01_(float v) { return v < 0.f ? 0.0 : (v > 1.f ? 1.0 : (double)v); }

    void design_() {
        // --- Delay lengths: scale the mutually-prime base lengths by Size ----
        // kBaseLen are prime sample counts at kDesignSR spanning ~10..45 ms
        // (small room). Size scales them 1.0 .. kMaxSizeScale (bigger room).
        const double srScale = sr_ / kDesignSR;
        const double sizeScale = 1.0 + size_ * (kMaxSizeScale - 1.0);
        for (int i = 0; i < kLines; ++i) {
            int len = static_cast<int>(std::lround(kBaseLen[i] * sizeScale * srScale));
            if (len < 2) len = 2;
            const int cap = static_cast<int>(line_[i].buf.size());
            if (len > cap) len = cap;       // clamp (RT safety; never realloc)
            line_[i].len = len;
        }

        // --- RT60 from Size, then per-line feedback gain ---------------------
        // Size maps to RT60: ~0.4 s (small) .. ~2.0 s (large room).
        const double rt60 = kRt60Min + size_ * (kRt60Max - kRt60Min);
        for (int i = 0; i < kLines; ++i) {
            const double dt = line_[i].len / sr_;          // delay in seconds
            // g = 10^(-3 * dt / RT60): each line loses 60 dB over RT60 seconds.
            double g = std::pow(10.0, -3.0 * dt / rt60);
            // Stability margin: the damping LPF has unity DC gain, so the loop
            // gain at DC is g; keep it strictly < 1.
            if (g > 0.9995) g = 0.9995;
            fb_gain_[i] = g;
        }

        // --- Damping one-pole LPF coefficient --------------------------------
        // y += a*(x - y); a = 1 - exp(-2*pi*fc/sr). DC gain is exactly 1, so it
        // never adds loop gain (stability preserved); it only rolls off HF.
        double fc = damp_hz_;
        if (fc < 200.0) fc = 200.0;
        if (fc > 0.49 * sr_) fc = 0.49 * sr_;
        damp_a_ = 1.0 - std::exp(-2.0 * M_PI * fc / sr_);

        // --- Pre-delay length, auto-scaled from Size (~8..30 ms) -------------
        const double pdms = kPredelayMinMs + size_ * (kMaxPredelayMs - kPredelayMinMs);
        pd_len_ = static_cast<int>(std::lround(pdms * 1e-3 * sr_));
        const int pdcap = static_cast<int>(predelay_[0].size()) - 1;
        if (pd_len_ < 0) pd_len_ = 0;
        if (pd_len_ > pdcap) pd_len_ = pdcap;

        // --- Reverb-send high-pass (2nd-order Butterworth, RBJ) --------------
        double lc = lowcut_hz_;
        if (lc < 20.0) lc = 20.0;
        if (lc > 0.49 * sr_) lc = 0.49 * sr_;
        const double w0 = 2.0 * M_PI * lc / sr_;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double Q  = 0.70710678;
        const double alpha = sw / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        const double hb0 = (1.0 + cw) / 2.0 / a0;
        const double hb1 = -(1.0 + cw) / a0;
        const double ha1 = -2.0 * cw / a0;
        const double ha2 = (1.0 - alpha) / a0;
        send_hpf_[0].set(hb0, hb1, hb0, ha1, ha2);
        send_hpf_[1].set(hb0, hb1, hb0, ha1, ha2);

        // --- Wet normalisation (approx energy) -------------------------------
        // Bigger/darker rooms ring louder for the same Mix. Scale the wet so a
        // given Mix lands at roughly consistent loudness across Size/Damp,
        // keeping the effect loudness-gentle. Empirical, gentle taper.
        // Larger RT60 → more buildup → trim down a little.
        wet_norm_ = kWetBase / (1.0 + 0.6 * size_);
    }

    // -- Design-time constants -------------------------------------------------
    static constexpr double kDesignSR     = 44100.0;
    static constexpr double kMaxSizeScale = 3.5;     // delay-length growth at Size=1
    static constexpr double kRt60Min      = 0.40;    // s  (small tasteful room)
    static constexpr double kRt60Max      = 2.00;    // s  (large room)
    static constexpr double kPredelayMinMs = 8.0;
    static constexpr double kMaxPredelayMs = 30.0;
    static constexpr double kWetBase      = 0.85;    // base wet trim (loudness-gentle)
    // Tap normalisation so the decorrelated combos don't sum hotter than mono.
    static constexpr double kTapNorm      = 1.0 / kLines;

    // Mutually-prime delay lengths (samples @ 44.1 kHz), ~10..45 ms. Primes
    // avoid coincident echo periods (= flutter) and keep the modal density
    // smooth/colourless.
    static constexpr std::array<int, kLines> kBaseLen = {
        443, 587, 691, 829, 947, 1109, 1303, 1481
    };
    // Decorrelation tap sign patterns: two different orthogonal-ish ±1 rows of
    // a Hadamard-like matrix. R is NOT the negation of L (that would phase-
    // invert and cancel in mono) — they differ in pattern, not global sign.
    static constexpr std::array<double, kLines> kTapL = {
        +1, +1, +1, +1, -1, -1, -1, -1
    };
    static constexpr std::array<double, kLines> kTapR = {
        +1, -1, +1, -1, +1, -1, +1, -1
    };

    double sr_ = 44100.0;
    float  mix_   = 0.f;
    float  size_  = 0.30f;
    float  width_ = 0.80f;
    float  damp_hz_   = 7000.f;
    float  lowcut_hz_ = 250.f;
    bool   coeffs_dirty_ = true;

    std::array<Line, kLines>      line_{};
    std::array<double, kLines>    fb_gain_{};
    double damp_a_   = 0.0;
    double in_gain_  = 0.5;       // send injection gain into the network
    double wet_norm_ = kWetBase;

    std::array<std::vector<double>, 2> predelay_{};
    int    pd_idx_ = 0;
    int    pd_len_ = 0;

    std::array<Biquad, 2> send_hpf_{};
};

}  // namespace nablafx
