// Shared STFT scaffolding for the vDSP-backed spectral units.
//
// Every STFT host (SpectralMaskEq, AdaptiveEq's RunningMelSpectrum,
// MelLimiter, CoherenceDistortion, the GUI analyzer) carried its own copy of
// the same fragments: the Hann build loop, the two-segment ring windowing +
// forward zrip FFT, and — for the two OLA synthesizers — the Hann²-OLA
// accumulate/drain machinery. These helpers are the verbatim expressions and
// vDSP call sequences from those sites — pure code motion, so FP results are
// unchanged and renders stay byte-identical.

#pragma once
#include "accelerate_shim.hpp"  // vDSP on macOS; portable pffft-backed shim elsewhere

#include <algorithm>
#include <cmath>
#include <vector>

namespace nablafx {

// Hann analysis window — the exact shared expression (associativity preserved).
inline void make_hann(float* w, int n) {
    for (int i = 0; i < n; ++i)
        w[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / n));
}

// Window an n_fft analysis ring (oldest sample first) into `out`, vectorized
// via two contiguous vDSP_vmul segments. `fill` is the ring's write index ==
// the position of the OLDEST sample.
inline void window_ring_oldest_first(const float* ring, int fill, int n_fft,
                                     const float* window, float* out) {
    const int tail = n_fft - fill;
    vDSP_vmul(ring + fill, 1, window, 1, out, 1, static_cast<vDSP_Length>(tail));
    if (fill > 0)
        vDSP_vmul(ring, 1, window + tail, 1, out + tail, 1,
                  static_cast<vDSP_Length>(fill));
}

// Pack a real windowed frame into split-complex form and run the forward real
// FFT (vDSP zrip packing: realp[0] = DC, imagp[0] = Nyquist).
inline void forward_zrip(FFTSetup setup, vDSP_Length log2n, int n_fft,
                         const float* windowed, DSPSplitComplex* split) {
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(windowed), 2, split, 1,
              static_cast<vDSP_Length>(n_fft / 2));
    vDSP_fft_zrip(setup, split, 1, log2n, kFFTDirection_Forward);
}

// Hann²-OLA output machinery shared by the two STFT synthesizers
// (SpectralMaskEq, MelLimiter): an audio accumulator ring + a window²
// normalizer ring, per-frame accumulate via two contiguous vDSP_vadd
// segments, and a per-sample drain that divides by the accumulated window²
// (torch.istft-style normalization), zeroes the consumed slots and advances.
struct OlaAccumulator {
    std::vector<float> out_ring;    // OLA audio accumulator
    std::vector<float> norm_ring;   // OLA window² accumulator (same size)
    int write = 0;
    int read  = 0;
    int avail = 0;

    // (Re)size both rings and zero all state. Call from prepare/init only.
    void assign(int ring_sz) {
        out_ring.assign(ring_sz, 0.0f);
        norm_ring.assign(ring_sz, 0.0f);
        write = read = avail = 0;
    }

    // Zero rings + counters without reallocating (audio-thread-safe reset).
    void clear() {
        std::fill(out_ring.begin(), out_ring.end(), 0.0f);
        std::fill(norm_ring.begin(), norm_ring.end(), 0.0f);
        write = read = avail = 0;
    }

    // Accumulate one synthesis frame (already windowed AND scaled by the
    // 1/(2N) vDSP round-trip factor) plus its window² normalizer.
    // `clamp_avail` guards `avail` against ring overflow when the reader
    // stalls, dropping the oldest unread samples (SpectralMaskEq's policy).
    // MelLimiter historically has no guard; the policy stays per-site so its
    // renders are byte-identical (unifying it would be a behavior change).
    void add_frame(const float* windowed_scaled, const float* window_sq,
                   int n_fft, int hop, bool clamp_avail) {
        const int ring_sz = static_cast<int>(out_ring.size());
        const int seg1    = std::min(n_fft, ring_sz - write);
        const int seg2    = n_fft - seg1;
        vDSP_vadd(windowed_scaled,   1, out_ring.data()  + write, 1,
                  out_ring.data()  + write, 1, static_cast<vDSP_Length>(seg1));
        vDSP_vadd(window_sq,         1, norm_ring.data() + write, 1,
                  norm_ring.data() + write, 1, static_cast<vDSP_Length>(seg1));
        if (seg2 > 0) {
            vDSP_vadd(windowed_scaled + seg1, 1, out_ring.data(),  1,
                      out_ring.data(),  1, static_cast<vDSP_Length>(seg2));
            vDSP_vadd(window_sq + seg1,       1, norm_ring.data(), 1,
                      norm_ring.data(), 1, static_cast<vDSP_Length>(seg2));
        }
        write = (write + hop) % ring_sz;
        if (clamp_avail) {
            if (avail + hop <= ring_sz) avail += hop;
            else                        avail = ring_sz;
        } else {
            avail += hop;
        }
    }

    // Pull one finished sample: divide by accumulated window² (guarding
    // near-zero norm at Hann window edges), zero the consumed slots, advance
    // the read pointer. Returns 0 while no output is available (start-up).
    float pull_sample() {
        if (avail <= 0) return 0.0f;
        const int   rd   = read;
        const float norm = norm_ring[rd];
        const float s    = (norm > 1e-8f) ? (out_ring[rd] / norm) : 0.0f;
        out_ring[rd]  = 0.0f;
        norm_ring[rd] = 0.0f;
        read = (rd + 1) % static_cast<int>(out_ring.size());
        --avail;
        return s;
    }
};

}  // namespace nablafx
