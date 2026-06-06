// Direct magnitude-spectrum meter for the exciter's WET (added) contribution.
//
// The exciter overlay must show EXACTLY what the exciter adds onto the dry: the
// band-limited, high-passed, shaped signal it sums in (= amount * wet per
// sample). Earlier overlays derived this from a post-minus-pre spectrum
// DIFFERENCE of the per-stage analyzer; that subtraction blew up in loud
// regions (the bass) where tiny pre/post jitter — amplified by the exciter's
// 32-sample dry delay — read as large "added energy", giving phantom low-end
// spikes, while the genuine high-band excitation was under-shown.
//
// This class instead measures the wet tap DIRECTLY: feed it the mono wet
// contribution (Exciter::process's wet_mono_out), window + FFT it, EMA-smooth
// the per-bin power, and reduce to a compact LOG-spaced magnitude array in
// dBFS. Below the HPF and above the harmonics the wet is ~0, so the overlay is
// correctly empty there — no subtraction, no loud-low-end artifact.
//
// FFT: size 1024, Hann window, 50% overlap (hop 512). Per-bin power EMA with a
// ~120 ms time constant. Output: kNumBins (= 56) log-spaced points spanning
// 20 Hz .. 20 kHz, each the dBFS magnitude (20*log10(|X|)) interpolated from
// the FFT bins, floored at kFloorDb.
//
// Backed by Apple Accelerate vDSP real FFT, mirroring coherence_distortion.hpp
// / spectral_mask_eq.hpp split-complex packing (bin 0 holds DC in realp[0],
// Nyquist in imagp[0]).
//
// Pure DSP — no CLAP/ORT deps so it can be unit-tested standalone. The FFT
// setup + all buffers are allocated in prepare(); push() is allocation-free and
// only runs arithmetic + the periodic FFT, so idle CPU (no pushes) is ~0.

#pragma once

#include <Accelerate/Accelerate.h>

#include <array>
#include <cmath>
#include <vector>

namespace nablafx {

class ExciterSpectrum {
public:
    // Compact output: log-spaced magnitude points for the UI overlay. The exact
    // frequencies are reproducible on the UI side via the same log formula
    // (see freq_for_bin / the documented FLO/FHI/kNumBins below).
    static constexpr int   kNumBins = 56;
    static constexpr float kFloLog  = 20.f;     // Hz, output point 0
    static constexpr float kFhiLog  = 20000.f;  // Hz, output point kNumBins-1
    static constexpr float kFloorDb = -96.f;    // reported when a point is silent

    // The i-th output point's centre frequency (log-spaced, inclusive ends).
    static constexpr float freq_for_bin(int i) {
        return kFloLog *
            std::pow(kFhiLog / kFloLog, (float)i / (float)(kNumBins - 1));
    }

    ExciterSpectrum() = default;
    ~ExciterSpectrum() {
        if (fft_setup_) vDSP_destroy_fftsetup(fft_setup_);
    }
    ExciterSpectrum(const ExciterSpectrum&)            = delete;
    ExciterSpectrum& operator=(const ExciterSpectrum&) = delete;

    // Allocate FFT setup + all buffers. Call from activate/prepare only.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        n_freq_      = kNfft / 2 + 1;
        log2_nfft_   = static_cast<vDSP_Length>(std::log2((double)kNfft));

        if (fft_setup_) vDSP_destroy_fftsetup(fft_setup_);
        fft_setup_ = vDSP_create_fftsetup(log2_nfft_, kFFTRadix2);

        window_.assign(kNfft, 0.f);
        // Coherent gain of the Hann window (sum of taps) — divide the FFT
        // magnitude by it so a full-scale tone reads ~0 dBFS regardless of N.
        double wsum = 0.0;
        for (int n = 0; n < kNfft; ++n) {
            window_[n] = 0.5f * (1.f - std::cos(2.f * (float)M_PI * n / kNfft));
            wsum += window_[n];
        }
        win_gain_ = (wsum > 0.0) ? (float)wsum : 1.f;

        in_.assign(kNfft, 0.f);
        wbuf_.assign(kNfft, 0.f);
        xr_.assign(kNfft / 2, 0.f);
        xi_.assign(kNfft / 2, 0.f);
        Pxx_.assign(n_freq_, 0.f);

        // EMA coefficient for ~120 ms: a = 1 - exp(-hop / (tau * sr)).
        ema_a_ = 1.f - std::exp(-(float)kHop / (kTauSec * (float)sample_rate_));

        // Precompute, for each log output point, the fractional FFT bin it maps
        // to (linear interpolation between adjacent magnitude bins).
        const float bin_hz = (float)sample_rate_ / (float)kNfft;
        for (int i = 0; i < kNumBins; ++i) {
            float f   = freq_for_bin(i);
            float fb  = f / bin_hz;                 // fractional FFT bin
            if (fb < 0.f) fb = 0.f;
            if (fb > (float)(n_freq_ - 1)) fb = (float)(n_freq_ - 1);
            map_bin_[i]  = (int)fb;
            map_frac_[i] = fb - (float)map_bin_[i];
            if (map_bin_[i] >= n_freq_ - 1) {
                map_bin_[i]  = n_freq_ - 1;
                map_frac_[i] = 0.f;
            }
        }
        reset();
    }

    // Clear the spectral average + accumulator. Cheap; safe on the audio thread.
    // Call when the stage is (re)enabled so re-enabling starts clean.
    void reset() {
        std::fill(in_.begin(), in_.end(), 0.f);
        std::fill(Pxx_.begin(), Pxx_.end(), 0.f);
        out_db_.fill(kFloorDb);
        fill_       = 0;
        since_      = 0;
        have_value_ = false;
    }

    // Push one block of mono wet samples (the exciter's added contribution).
    // Runs zero or more FFT frames internally (every kHop samples). No alloc.
    void push(const float* wet_mono, int n) {
        for (int i = 0; i < n; ++i) {
            in_[fill_] = wet_mono[i];
            fill_ = (fill_ + 1) % kNfft;
            if (++since_ >= kHop) {
                since_ -= kHop;
                run_frame_();
            }
        }
    }

    bool has_value() const { return have_value_; }

    // Copy the current log-spaced dBFS spectrum into dst[kNumBins].
    void copy_spectrum(float* dst) const {
        for (int i = 0; i < kNumBins; ++i) dst[i] = out_db_[i];
    }

private:
    void run_frame_() {
        // Window the ring (oldest-first) into wbuf_, then pack + forward FFT.
        const int tail = kNfft - fill_;
        vDSP_vmul(in_.data() + fill_, 1, window_.data(), 1, wbuf_.data(), 1,
                  (vDSP_Length)tail);
        if (fill_ > 0)
            vDSP_vmul(in_.data(), 1, window_.data() + tail, 1,
                      wbuf_.data() + tail, 1, (vDSP_Length)fill_);

        DSPSplitComplex sx{xr_.data(), xi_.data()};
        vDSP_ctoz(reinterpret_cast<DSPComplex*>(wbuf_.data()), 2, &sx, 1,
                  kNfft / 2);
        vDSP_fft_zrip(fft_setup_, &sx, 1, log2_nfft_, kFFTDirection_Forward);

        // zrip packing: realp[0]=DC, imagp[0]=Nyquist, realp[k]+i*imagp[k] = bin
        // k for 0<k<N/2. vDSP scales the forward transform by 2; fold that out
        // with the 0.5 below so the per-bin power is calibrated. EMA the power.
        const float a  = ema_a_;
        const float ia = 1.f - a;
        auto pwr = [&](float re, float im) {
            const float v = 0.5f * (re * re + im * im);
            return v;
        };
        Pxx_[0]           = ia * Pxx_[0]           + a * pwr(sx.realp[0], 0.f);
        Pxx_[n_freq_ - 1] = ia * Pxx_[n_freq_ - 1] + a * pwr(sx.imagp[0], 0.f);
        for (int k = 1; k < kNfft / 2; ++k)
            Pxx_[k] = ia * Pxx_[k] + a * pwr(sx.realp[k], sx.imagp[k]);

        // Reduce the smoothed power spectrum to the log-spaced dBFS output.
        // Magnitude = sqrt(power); normalise by window coherent gain so a
        // full-scale sine reads ~0 dBFS. Interpolate between adjacent FFT bins.
        const float inv_wg = 1.f / win_gain_;
        for (int i = 0; i < kNumBins; ++i) {
            const int   b  = map_bin_[i];
            const float fr = map_frac_[i];
            const float p0 = Pxx_[b];
            const float p1 = (b + 1 < n_freq_) ? Pxx_[b + 1] : p0;
            const float p  = p0 + fr * (p1 - p0);          // interp power
            const float mag = std::sqrt(std::max(p, 0.f)) * 2.f * inv_wg;
            float db = (mag > 1e-9f) ? 20.f * std::log10(mag) : kFloorDb;
            if (db < kFloorDb) db = kFloorDb;
            out_db_[i] = db;
        }
        have_value_ = true;
    }

    static constexpr int   kNfft   = 1024;
    static constexpr int   kHop    = 512;     // 50% overlap
    static constexpr float kTauSec = 0.12f;   // ~120 ms per-bin power EMA

    double      sample_rate_{48000.0};
    int         n_freq_{0};
    vDSP_Length log2_nfft_{0};
    FFTSetup    fft_setup_{nullptr};
    float       ema_a_{0.f};
    float       win_gain_{1.f};

    std::vector<float> window_;
    std::vector<float> in_;        // kNfft analysis ring
    int                fill_{0};
    int                since_{0};

    std::vector<float> wbuf_;      // windowed scratch
    std::vector<float> xr_, xi_;   // split-complex scratch
    std::vector<float> Pxx_;       // running per-bin power (EMA)

    std::array<int,   kNumBins> map_bin_{};
    std::array<float, kNumBins> map_frac_{};
    std::array<float, kNumBins> out_db_{};
    bool                        have_value_{false};
};

}  // namespace nablafx
