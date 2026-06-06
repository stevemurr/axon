// Phase-invariant distortion meter via magnitude-squared coherence.
//
// The bus comp is a learned streaming TCN. It phase-rotates the signal, so a
// naive residual rms(wet - dry) is pinned near 0 dB even when the model is
// barely altering the audio — the phase shift alone produces a large residual.
//
// Magnitude-squared coherence between the model's input (dry) and output (wet)
// discards phase entirely:
//
//   γ²(f) = |Sxy(f)|² / (Sxx(f) · Syy(f))   (averaged over time)
//
//     Sxx = ⟨|X|²⟩, Syy = ⟨|Y|²⟩, Sxy = ⟨X·conj(Y)⟩,  X=FFT(dry), Y=FFT(wet)
//
// γ² → 1 where the output is a LINEAR (possibly phase-shifted) function of the
// input. γ² < 1 is the part NOT linearly explained: distortion, nonlinearity,
// and the time-varying compression. So (1 − γ²) is a phase-invariant "how much
// is the model altering the signal" measure — exactly the "crunch" we want.
//
// CRITICAL: coherence is trivially 1 for a single frame. It only becomes
// meaningful when Sxx/Syy/Sxy are averaged over multiple frames. We keep a
// running complex EMA (~200 ms time constant) across frames.
//
// FFT: size 1024, Hann window, 50% overlap (hop 512). Mono sum 0.5*(L+R) of
// dry and of wet is accumulated across the host's 128-sample blocks; every 512
// new samples one frame is windowed + FFT'd for both signals and the EMAs are
// updated.
//
// Backed by Apple Accelerate vDSP real FFT, mirroring spectral_mask_eq.hpp's
// split-complex packing (bin 0 holds DC in realp[0] and Nyquist in imagp[0]).
//
// Pure DSP — no CLAP/ORT deps so it can be unit-tested standalone. All buffers
// + the FFT setup are allocated in prepare(); process is allocation-free.

#pragma once

#include <Accelerate/Accelerate.h>

#include <cmath>
#include <vector>

namespace nablafx {

class CoherenceDistortion {
public:
    CoherenceDistortion() = default;
    ~CoherenceDistortion() {
        if (fft_setup_) vDSP_destroy_fftsetup(fft_setup_);
    }

    CoherenceDistortion(const CoherenceDistortion&)            = delete;
    CoherenceDistortion& operator=(const CoherenceDistortion&) = delete;

    // Allocate FFT setup + all buffers. Call from activate/prepare only.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate;
        n_freq_      = kNfft / 2 + 1;
        log2_nfft_   = static_cast<vDSP_Length>(std::log2((double)kNfft));

        if (fft_setup_) vDSP_destroy_fftsetup(fft_setup_);
        fft_setup_ = vDSP_create_fftsetup(log2_nfft_, kFFTRadix2);

        window_.assign(kNfft, 0.f);
        for (int n = 0; n < kNfft; ++n)
            window_[n] = 0.5f * (1.f - std::cos(2.f * (float)M_PI * n / kNfft));

        in_dry_.assign(kNfft, 0.f);
        in_wet_.assign(kNfft, 0.f);

        wx_.assign(kNfft, 0.f);
        wy_.assign(kNfft, 0.f);
        xr_.assign(kNfft / 2, 0.f);
        xi_.assign(kNfft / 2, 0.f);
        yr_.assign(kNfft / 2, 0.f);
        yi_.assign(kNfft / 2, 0.f);

        Sxx_.assign(n_freq_, 0.f);
        Syy_.assign(n_freq_, 0.f);
        Sxy_re_.assign(n_freq_, 0.f);
        Sxy_im_.assign(n_freq_, 0.f);

        // EMA coefficient for ~200 ms: a = 1 - exp(-hop / (tau * sr)).
        ema_a_ = 1.f - std::exp(-(float)kHop / (kTauSec * (float)sample_rate));

        // Band limits in bins [~100 Hz, ~16 kHz], clamped to valid range.
        const float bin_hz = (float)sample_rate / (float)kNfft;
        lo_bin_ = (int)std::ceil(100.f / bin_hz);
        hi_bin_ = (int)std::floor(16000.f / bin_hz);
        if (lo_bin_ < 1)            lo_bin_ = 1;
        if (hi_bin_ > n_freq_ - 1)  hi_bin_ = n_freq_ - 1;

        reset();
    }

    // Clear the spectral averages + accumulator. Cheap; safe on the audio
    // thread. Call when the stage is (re)enabled so re-enabling starts clean.
    void reset() {
        std::fill(in_dry_.begin(), in_dry_.end(), 0.f);
        std::fill(in_wet_.begin(), in_wet_.end(), 0.f);
        std::fill(Sxx_.begin(), Sxx_.end(), 0.f);
        std::fill(Syy_.begin(), Syy_.end(), 0.f);
        std::fill(Sxy_re_.begin(), Sxy_re_.end(), 0.f);
        std::fill(Sxy_im_.begin(), Sxy_im_.end(), 0.f);
        fill_       = 0;
        since_      = 0;
        frames_     = 0;
        last_db_    = kFloorDb;
        have_value_ = false;
    }

    // Push one block of time-aligned mono dry + wet samples. Runs zero or more
    // FFT frames internally (every kHop samples). Allocation-free.
    void push(const float* dry_mono, const float* wet_mono, int n) {
        for (int i = 0; i < n; ++i) {
            in_dry_[fill_] = dry_mono[i];
            in_wet_[fill_] = wet_mono[i];
            fill_ = (fill_ + 1) % kNfft;
            if (++since_ >= kHop) {
                since_ -= kHop;
                run_frame_();
            }
        }
    }

    // Current distortion as a power-ratio dB in [kFloorDb, 0]. 0 dB = heavily
    // altered, kFloorDb = perfectly linear / idle. Returns floor until enough
    // frames have been averaged (warm-up guard).
    float distortion_db() const {
        return have_value_ ? last_db_ : kFloorDb;
    }
    bool has_value() const { return have_value_; }

    static constexpr float floor_db() { return kFloorDb; }

private:
    void run_frame_() {
        // Window the rings (oldest-first) into wx_/wy_, then pack + forward FFT.
        const int tail = kNfft - fill_;
        vDSP_vmul(in_dry_.data() + fill_, 1, window_.data(), 1, wx_.data(), 1, (vDSP_Length)tail);
        vDSP_vmul(in_wet_.data() + fill_, 1, window_.data(), 1, wy_.data(), 1, (vDSP_Length)tail);
        if (fill_ > 0) {
            vDSP_vmul(in_dry_.data(), 1, window_.data() + tail, 1, wx_.data() + tail, 1, (vDSP_Length)fill_);
            vDSP_vmul(in_wet_.data(), 1, window_.data() + tail, 1, wy_.data() + tail, 1, (vDSP_Length)fill_);
        }

        // GATE: skip the EMA update on near-silent frames so the floor stays
        // clean and the meter holds rather than flashing garbage. Use the dry
        // (model input) mono energy as the activity reference.
        float dry_energy = 0.f;
        vDSP_svesq(wx_.data(), 1, &dry_energy, (vDSP_Length)kNfft);
        if (dry_energy < kEnergyFloor) return;

        DSPSplitComplex sx{xr_.data(), xi_.data()};
        DSPSplitComplex sy{yr_.data(), yi_.data()};
        vDSP_ctoz(reinterpret_cast<DSPComplex*>(wx_.data()), 2, &sx, 1, kNfft / 2);
        vDSP_ctoz(reinterpret_cast<DSPComplex*>(wy_.data()), 2, &sy, 1, kNfft / 2);
        vDSP_fft_zrip(fft_setup_, &sx, 1, log2_nfft_, kFFTDirection_Forward);
        vDSP_fft_zrip(fft_setup_, &sy, 1, log2_nfft_, kFFTDirection_Forward);

        // zrip packing: realp[0]=X(DC), imagp[0]=X(Nyquist), realp[k]+i*imagp[k]
        // = bin k for 0<k<N/2. Build a helper that yields (Xre,Xim,Yre,Yim) per
        // bin and update the four EMAs. (vDSP scales the forward transform by 2;
        // the constant factor cancels in γ², so we don't normalise.)
        const float a  = ema_a_;
        const float ia = 1.f - a;
        auto upd = [&](int k, float Xre, float Xim, float Yre, float Yim) {
            Sxx_[k]    = ia * Sxx_[k]    + a * (Xre * Xre + Xim * Xim);
            Syy_[k]    = ia * Syy_[k]    + a * (Yre * Yre + Yim * Yim);
            Sxy_re_[k] = ia * Sxy_re_[k] + a * (Xre * Yre + Xim * Yim);
            Sxy_im_[k] = ia * Sxy_im_[k] + a * (Xim * Yre - Xre * Yim);
        };
        upd(0,            sx.realp[0], 0.f, sy.realp[0], 0.f);          // DC
        upd(n_freq_ - 1,  sx.imagp[0], 0.f, sy.imagp[0], 0.f);         // Nyquist
        for (int k = 1; k < kNfft / 2; ++k)
            upd(k, sx.realp[k], sx.imagp[k], sy.realp[k], sy.imagp[k]);

        ++frames_;
        if (frames_ < kWarmupFrames) return;   // warm-up guard

        // Energy-weighted, band-limited reduction over [lo_bin_, hi_bin_].
        double num = 0.0, den = 0.0;
        constexpr float eps = 1e-20f;
        for (int k = lo_bin_; k <= hi_bin_; ++k) {
            const float mag2 = Sxy_re_[k] * Sxy_re_[k] + Sxy_im_[k] * Sxy_im_[k];
            float g2 = mag2 / (Sxx_[k] * Syy_[k] + eps);
            if (g2 < 0.f) g2 = 0.f;
            if (g2 > 1.f) g2 = 1.f;
            const float w = Syy_[k];   // weight by output power
            num += (double)w * (1.f - g2);
            den += (double)w;
        }
        const float distortion = (den > 0.0) ? (float)(num / den) : 0.f;
        float db = 10.f * std::log10(std::max(distortion, 1e-5f));  // power ratio
        if (db < kFloorDb) db = kFloorDb;
        if (db > 0.f)      db = 0.f;
        last_db_    = db;
        have_value_ = true;
    }

    static constexpr int   kNfft         = 1024;
    static constexpr int   kHop          = 512;     // 50% overlap
    static constexpr float kTauSec       = 0.2f;    // ~200 ms EMA
    static constexpr int   kWarmupFrames = 8;       // hold floor until averaged
    static constexpr float kFloorDb      = -48.f;
    static constexpr float kEnergyFloor  = 1e-7f;   // near-silence gate (windowed sum-sq)

    double      sample_rate_{48000.0};
    int         n_freq_{0};
    int         lo_bin_{1}, hi_bin_{1};
    vDSP_Length log2_nfft_{0};
    FFTSetup    fft_setup_{nullptr};
    float       ema_a_{0.f};

    std::vector<float> window_;
    std::vector<float> in_dry_, in_wet_;   // kNfft analysis rings
    int                fill_{0};
    int                since_{0};
    int                frames_{0};

    std::vector<float> wx_, wy_;           // windowed scratch
    std::vector<float> xr_, xi_, yr_, yi_; // split-complex scratch

    std::vector<float> Sxx_, Syy_, Sxy_re_, Sxy_im_;  // running spectra

    float last_db_{kFloorDb};
    bool  have_value_{false};
};

}  // namespace nablafx
