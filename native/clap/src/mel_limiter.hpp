// Adaptive multiband limiter: 26-band Mel-scale STFT filterbank with a
// per-hop constrained gain solver (water-filling / uniform blend).
//
// Architecture mirrors Newfangled Audio Elevate:
//   analysis STFT → per-band energy → water-filling gain solve →
//   time-smoothed gains → per-bin gain apply → IFFT + OLA synthesis
//
// Uses Apple Accelerate vDSP for FFT (macOS arm64 only).
// Latency: kFFTSize samples (1024 ≈ 23 ms at 44 100 Hz).

#pragma once
#include <Accelerate/Accelerate.h>
#include <array>
#include <cmath>
#include <vector>

namespace nablafx {

class MelLimiter {
public:
    static constexpr int kFFTSize  = 1024;
    static constexpr int kHopSize  = 256;
    static constexpr int kNumBands = 26;
    static constexpr int kBrickLA  = 256;                 // brickwall lookahead (~5.8 ms)
    static constexpr int kLatency  = kFFTSize + kBrickLA; // samples; report to host

    struct Params {
        float ceiling_lin{1.f};      // true-peak output ceiling (linear amplitude)
        float drive_lin{1.f};        // input gain (≥1) — pushes signal into ceiling
        float adaptive_gain{0.5f};   // 0 = uniform limiting, 1 = max water-fill
        float adaptive_speed{0.5f};  // release time [0..1] → [30..400 ms]
        float wet_mix{1.f};          // wet/dry bypass
        // When true, the adaptive_gain/adaptive_speed controls also shape the
        // brickwall release (program-dependent, "dynamic" — Elevate-like).
        // When false, the brickwall uses a fixed fast release ("even").
        bool  adaptive_brickwall{false};
    };

    MelLimiter();
    ~MelLimiter();
    MelLimiter(const MelLimiter&)            = delete;
    MelLimiter& operator=(const MelLimiter&) = delete;

    void init(int sample_rate);
    void reset();

    // Process n_samples in-place for up to 2 channels.
    // Gains are linked across channels (use max per-band energy for decisions).
    void process(float* l, float* r, int n_ch, int n_samples, const Params& p);

    // ── Display taps (main thread snapshots these; audio thread writes them) ──
    static constexpr int num_bands() { return kNumBands; }
    // Copy the latest per-band display state. Pointers must hold kNumBands each:
    //   levels_lin  — measured (driven) band level, linear amplitude
    //   gains_lin   — smoothed per-band gain applied (1 = no limiting)
    //   centers_hz  — band centre frequencies (static after init)
    void copy_display(float* levels_lin, float* gains_lin, float* centers_hz) const;

    // Current brickwall gain (1 = no peak limiting, <1 = reducing). Linked stereo.
    float brickwall_gain() const { return brick_gain_; }

private:
    int sr_{44100};

    FFTSetup    fft_setup_{nullptr};
    vDSP_Length log2n_{0};

    std::vector<float> window_;
    std::vector<float> window_sq_;

    // Per-channel state (up to 2).
    struct ChState {
        std::vector<float> in_ring;
        int                in_fill{0};
        int                samples_since{0};
        std::vector<float> out_ring;
        std::vector<float> norm_ring;
        int                out_write{0};
        int                out_read{0};
        int                out_avail{0};
        // The WOLA wet path has a natural group delay of kFFTSize-1; this
        // single-sample delay aligns it to the reported kLatency (kFFTSize)
        // and to the dry delay ring.
        float              wet_z1{0.f};
        // Brickwall lookahead delay line (size kBrickLA).
        std::vector<float> la_ring;
        // Dry delay ring aligned with kLatency.
        std::vector<float> dry_ring;
        int                dry_write{0};
        // FFT scratch (per-channel so channels can process in sequence).
        std::vector<float> windowed;
        std::vector<float> sp_re, sp_im;
        std::vector<float> time_out;
    };
    std::array<ChState, 2> ch_{};

    // Shared Mel filterbank.
    int                n_freq_{0};
    std::vector<float> band_to_bin_;   // [kNumBands * n_freq]
    std::vector<float> bin_norm_;      // [n_freq]

    // Shared per-band gain state (linked stereo).
    std::array<float, kNumBands> band_gain_{};

    // Display taps: latest measured band levels + static band centres (Hz).
    mutable std::array<float, kNumBands> disp_level_{};
    std::array<float, kNumBands>         band_center_hz_{};

    float ola_scale_{1.f};
    // Converts raw vDSP FFT-domain energy to linear-amplitude units so band
    // levels are comparable to ceiling_lin.  κ = 1/sqrt(N · Σwindow²); a
    // full-scale sine then yields total ≈ 1.0.  Without this the solver reads
    // every signal as ~N× over ceiling and crushes all audio.
    float level_scale_{1.f};

    // Brickwall limiter state (linked stereo): lookahead gain envelope.
    int   la_pos_{0};
    float brick_gain_{1.f};
    float brick_atk_{0.f};   // attack smoothing coeff (reaches target < kBrickLA)
    float brick_rel_{0.f};   // release smoothing coeff

    // Sliding-window peak detector (monotonic deque) over the lookahead window,
    // so the gain targets the loudest upcoming sample rather than just one.
    static constexpr int kDqCap = kBrickLA + 1;
    std::array<float, kDqCap>   dq_val_{};   // window maxima, decreasing front→back
    std::array<long long, kDqCap> dq_idx_{}; // sample index of each entry
    int       dq_head_{0}, dq_tail_{0};      // ring; empty when head == tail
    long long brick_n_{0};                   // running input-sample counter

    void build_mel_();
    // True-peak brickwall on the wet output (linked). Reads the just-produced
    // wet samples in[], writes capped samples out[] delayed by kBrickLA.
    void brickwall_(const float* in, float* out, int n_ch, float ceiling);

    // Compute linked per-band target gains from L (and R if n_ch==2).
    void solve_gains_(float* const* ch_sp_re,
                      float* const* ch_sp_im,
                      int n_ch,
                      const Params& p,
                      float* out_gains) const;
};

}  // namespace nablafx
