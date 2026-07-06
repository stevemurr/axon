// Adaptive-EQ CONTROLLER A/B foundation.
//
// The renderer (IirFilterbankEq / SpectralMaskEq) already exists and turns a
// vector of per-band gain targets into a magnitude curve. This header is the
// shared contract + shared DSP for the *controllers* — the decision logic that
// produces those per-band targets from the running input. Each candidate
// (C1 deterministic target-match, C2 dynamic resonance suppression, C3
// perceptual-weighted) implements IEqController against this contract; the A/B
// harness (harness_eq_control.cpp) drives them over a spectrally-distinct
// material battery and scores adaptivity / target-match / artifacts.
//
// Design notes for candidate authors:
//   - A controller is ANALYSIS-ONLY. observe() ingests input blocks and updates
//     internal state; target() reports the current per-band dB curve. It never
//     touches the audio path, so analysis latency (an STFT estimate that lags by
//     a window) does NOT add output latency — the zero-latency biquad renderer
//     applies the curve.
//   - Work in dB. The renderer maps dB→sigmoid via bands_from_db().
//   - All shared estimators live here so candidates compose, not reinvent, them
//     (keeps C1/C2/C3 comparable: same running-spectrum, same band centers).
//
// Header-only. Uses Accelerate (vDSP) for the analysis FFT — the harness links
// it already (matches spectral_mask_eq.hpp).
#pragma once

#include "accelerate_shim.hpp"  // via -I src: vDSP on macOS; portable shim elsewhere

#include <cmath>
#include <cstddef>
#include <vector>

namespace eqctrl {

// ----------------------------------------------------------------------------
// Config — mirrors the fields SpectralMaskEqParams exposes, so a controller and
// its renderer agree on band layout.
// ----------------------------------------------------------------------------
struct EqCtrlCfg {
    int   sample_rate = 44100;
    int   block_size  = 128;
    int   n_bands     = 24;
    float f_min       = 40.0f;
    float f_max       = 18000.0f;
    float min_gain_db = -12.0f;
    float max_gain_db = 12.0f;
};

// ----------------------------------------------------------------------------
// Perceptual-scale helpers (Glasberg & Moore 1990). cam = ERB-rate ("Cam").
// ----------------------------------------------------------------------------
inline double hz_to_cam(double f) { return 21.4 * std::log10(4.37e-3 * f + 1.0); }
inline double cam_to_hz(double c) { return (std::pow(10.0, c / 21.4) - 1.0) / 4.37e-3; }
inline double erb_hz(double f)    { return 24.7 * (4.37e-3 * f + 1.0); }   // ERB bandwidth (Hz)

// HTK-mel band centers, matching SpectralMaskEq::build_mel_ exactly so the
// controller's bands line up with the renderer's.
inline std::vector<double> mel_centers(const EqCtrlCfg& c) {
    const double mmin = 2595.0 * std::log10(1.0 + c.f_min / 700.0);
    const double mmax = 2595.0 * std::log10(1.0 + c.f_max / 700.0);
    std::vector<double> out(c.n_bands);
    for (int b = 0; b < c.n_bands; ++b) {
        const double mel = mmin + (mmax - mmin) * (b + 1) / (c.n_bands + 1);
        out[b] = 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
        out[b] = std::min(out[b], 0.49 * c.sample_rate);
    }
    return out;
}

// ----------------------------------------------------------------------------
// Empirical tonal-balance TARGET curve (Paradigm-1). Analytic stand-in for a
// corpus-measured reference: roughly flat below ~100 Hz, ~5 dB/oct decay
// 100 Hz→4 kHz (Ma/Reiss/Black AES134), gentler decay above. Returned RELATIVE
// (the solver zero-means it, so the absolute offset is irrelevant).
inline double target_db(double f) {
    const double knee_lo = 100.0, knee_hi = 4000.0;
    const double slope   = 5.0;                       // dB per octave, 100→4k
    if (f <= knee_lo) return 0.0;
    if (f <= knee_hi) return -slope * std::log2(f / knee_lo);
    const double at_hi = -slope * std::log2(knee_hi / knee_lo);
    return at_hi - 0.5 * slope * std::log2(f / knee_hi);  // half-slope above 4k
}

// Per-band tolerance half-width (dB) for the solver's deadband. Tonal-balance
// references are RANGES, not lines (iZotope TBC) — don't correct inside the band.
inline double tolerance_db(double f) {
    if (f < 80.0)    return 2.5;   // sub: wide tolerance
    if (f > 8000.0)  return 2.5;   // air: wide tolerance
    return 1.5;                    // mids: tighter
}

// Gaussian smoothing of a per-band dB curve (band-domain analogue of a
// low-quefrency cepstral lifter at this resolution). sigma in BANDS.
inline void gaussian_smooth_db(const float* in, float* out, int nb, double sigma) {
    if (sigma <= 1e-6) { for (int i = 0; i < nb; ++i) out[i] = in[i]; return; }
    const int half = (int)std::ceil(3.0 * sigma);
    for (int i = 0; i < nb; ++i) {
        double acc = 0.0, wsum = 0.0;
        for (int j = -half; j <= half; ++j) {
            const int k = i + j;
            if (k < 0 || k >= nb) continue;
            const double w = std::exp(-0.5 * (double)j * j / (sigma * sigma));
            acc += w * in[k]; wsum += w;
        }
        out[i] = (float)(wsum > 0 ? acc / wsum : in[i]);
    }
}

// Broadband trim (dB) to add to EVERY band of `gain_db` so the realized
// broadband energy is unchanged, given the per-band running spectrum `spec_db`.
// A zero-mean-in-dB curve is NOT loudness-neutral (dB↔power is nonlinear, so
// boosting high-energy bands raises loudness); this nets it out in the energy
// domain using the controller's own spectrum estimate (no output measurement).
inline double makeup_db(const float* gain_db, const float* spec_db, int nb) {
    double ein = 0.0, eout = 0.0;
    for (int b = 0; b < nb; ++b) {
        const double p = std::pow(10.0, (double)spec_db[b] / 10.0);   // relative band power
        ein  += p;
        eout += p * std::pow(10.0, (double)gain_db[b] / 10.0);
    }
    if (ein <= 1e-30 || eout <= 1e-30) return 0.0;
    return -10.0 * std::log10(eout / ein);
}

// Continuous deadband: shrink toward 0 by `tol` instead of hard-gating, so a
// band dithering across the tolerance edge produces a small continuous change
// rather than a 0↔±tol jump (kills the deadband-toggling zipper).
inline float soft_deadband(float v, float tol) {
    if (v >  tol) return v - tol;
    if (v < -tol) return v + tol;
    return 0.0f;
}

// Per-band noise gate (Ma/Reiss/Black per-frame-gate, band-domain variant):
// true if band b carries real program energy (within gate_db of the loudest
// band). Don't EQ near-silent bands — boosting the floor is the main source of
// steady-tone zipper and over-boost.
inline bool band_active(const float* spec_db, int nb, int b, double gate_db = 45.0) {
    float peak = -1e30f;
    for (int i = 0; i < nb; ++i) peak = std::max(peak, spec_db[i]);
    return spec_db[b] >= peak - (float)gate_db;
}

// ----------------------------------------------------------------------------
// Running mel-band magnitude spectrum: EWMA of windowed-FFT power, accumulated
// into HTK-mel bands, reported in dB. The shared estimator for all candidates.
// ----------------------------------------------------------------------------
class RunningMelSpectrum {
public:
    // tau_s = EWMA time constant in seconds (mastering: ~1–3 s).
    void reset(const EqCtrlCfg& cfg, int n_fft = 2048, int hop = 512, double tau_s = 2.0) {
        cfg_ = cfg; n_fft_ = n_fft; hop_ = hop;
        n_freq_ = n_fft_ / 2 + 1;
        log2_ = (vDSP_Length)std::lround(std::log2((double)n_fft_));
        if (fft_) vDSP_destroy_fftsetup(fft_);
        fft_ = vDSP_create_fftsetup(log2_, kFFTRadix2);

        window_.assign(n_fft_, 0.0f);
        for (int n = 0; n < n_fft_; ++n)
            window_[n] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * n / n_fft_));

        in_ring_.assign(n_fft_, 0.0f); in_fill_ = 0; since_ = 0;
        windowed_.assign(n_fft_, 0.0f);
        re_.assign(n_fft_ / 2, 0.0f); im_.assign(n_fft_ / 2, 0.0f);
        power_.assign(n_freq_, 0.0f);

        build_mel_();
        mel_db_.assign(cfg_.n_bands, -120.0f);
        primed_ = false;

        const double frames_per_tau = (cfg_.sample_rate * tau_s) / std::max(1, hop_);
        alpha_ = std::exp(-1.0 / std::max(1.0, frames_per_tau));
    }
    ~RunningMelSpectrum() { if (fft_) vDSP_destroy_fftsetup(fft_); }

    void observe(const float* in, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            in_ring_[in_fill_] = in[i];
            in_fill_ = (in_fill_ + 1) % n_fft_;
            if (++since_ >= hop_) { since_ -= hop_; frame_(); }
        }
    }

    // Current EWMA mel-dB curve (length n_bands).
    void mel_db(float* out) const { for (int b = 0; b < cfg_.n_bands; ++b) out[b] = mel_db_[b]; }
    const std::vector<double>& centers() const { return centers_; }
    bool primed() const { return primed_; }

private:
    void frame_() {
        const int tail = n_fft_ - in_fill_;
        vDSP_vmul(in_ring_.data() + in_fill_, 1, window_.data(), 1, windowed_.data(), 1, tail);
        if (in_fill_ > 0)
            vDSP_vmul(in_ring_.data(), 1, window_.data() + tail, 1, windowed_.data() + tail, 1, in_fill_);

        DSPSplitComplex sc{re_.data(), im_.data()};
        vDSP_ctoz(reinterpret_cast<DSPComplex*>(windowed_.data()), 2, &sc, 1, n_fft_ / 2);
        vDSP_fft_zrip(fft_, &sc, 1, log2_, kFFTDirection_Forward);

        // Power per bin. zrip packs DC in re[0], Nyquist in im[0].
        power_[0]          = re_[0] * re_[0];
        power_[n_freq_ - 1] = im_[0] * im_[0];
        for (int k = 1; k < n_fft_ / 2; ++k)
            power_[k] = re_[k] * re_[k] + im_[k] * im_[k];

        // Accumulate to mel bands as the weighted-MEAN power (power density),
        // NOT the sum: dividing by Σweights makes a flat input read as a flat
        // mel-dB curve, so in_db is directly comparable to a per-frequency
        // target. (A plain sum would tilt up with frequency as bands widen.)
        for (int b = 0; b < cfg_.n_bands; ++b) {
            double p = 0.0;
            const float* w = band_to_bin_.data() + (size_t)b * n_freq_;
            for (int k = 0; k < n_freq_; ++k) p += (double)w[k] * power_[k];
            p /= band_wsum_[b];
            const float db = 10.0f * std::log10((float)std::max(p, 1e-20));
            mel_db_[b] = primed_ ? (float)(alpha_ * mel_db_[b] + (1.0 - alpha_) * db) : db;
        }
        primed_ = true;
    }

    void build_mel_() {
        const double mel_min = 2595.0 * std::log10(1.0 + cfg_.f_min / 700.0);
        const double mel_max = 2595.0 * std::log10(1.0 + cfg_.f_max / 700.0);
        std::vector<double> binpts(cfg_.n_bands + 2);
        centers_.assign(cfg_.n_bands, 0.0);
        for (int i = 0; i < cfg_.n_bands + 2; ++i) {
            const double mel = mel_min + (mel_max - mel_min) * i / (cfg_.n_bands + 1);
            const double hz  = 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
            binpts[i] = std::min(std::max(hz * n_fft_ / cfg_.sample_rate, 0.0), (double)(n_freq_ - 1));
            if (i >= 1 && i <= cfg_.n_bands) centers_[i - 1] = hz;
        }
        band_to_bin_.assign((size_t)cfg_.n_bands * n_freq_, 0.0f);
        band_wsum_.assign(cfg_.n_bands, 1e-9f);
        for (int b = 0; b < cfg_.n_bands; ++b) {
            const double l = binpts[b], c = binpts[b + 1], r = binpts[b + 2];
            const double ls = std::max(c - l, 1e-6), rs = std::max(r - c, 1e-6);
            double ws = 0.0;
            for (int k = 0; k < n_freq_; ++k) {
                const double up = (k - l) / ls, dn = (r - k) / rs;
                float wv = (float)std::min(up, dn);
                wv = wv > 0 ? wv : 0.0f;
                band_to_bin_[(size_t)b * n_freq_ + k] = wv;
                ws += wv;
            }
            band_wsum_[b] = (float)std::max(ws, 1e-9);
        }
    }

    EqCtrlCfg cfg_{};
    int n_fft_ = 2048, hop_ = 512, n_freq_ = 1025, in_fill_ = 0, since_ = 0;
    vDSP_Length log2_ = 11;
    FFTSetup fft_ = nullptr;
    double alpha_ = 0.0; bool primed_ = false;
    std::vector<float> window_, in_ring_, windowed_, re_, im_, power_, band_to_bin_;
    std::vector<float> band_wsum_;   // Σ band_to_bin weights per band (for mean)
    std::vector<float> mel_db_;
    std::vector<double> centers_;
};

// ----------------------------------------------------------------------------
// Controller contract.
// ----------------------------------------------------------------------------
class IEqController {
public:
    virtual ~IEqController() = default;
    virtual const char* name() const = 0;
    virtual void reset(const EqCtrlCfg& cfg) = 0;
    virtual void observe(const float* in, std::size_t n) = 0;  // analysis
    virtual void target(float* per_band_db, int nb) = 0;       // current dB curve
};

// dB curve → renderer's [0,1] sigmoid band values (inverse of min+g*span).
inline void bands_from_db(const float* db, float* bands, int nb, const EqCtrlCfg& cfg) {
    const double span = cfg.max_gain_db - cfg.min_gain_db;
    for (int b = 0; b < nb; ++b) {
        double g = (db[b] - cfg.min_gain_db) / span;
        bands[b] = (float)std::min(std::max(g, 0.0), 1.0);
    }
}

}  // namespace eqctrl
