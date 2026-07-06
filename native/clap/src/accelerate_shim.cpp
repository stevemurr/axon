// accelerate_shim.cpp — pffft-backed implementation of the portable FFT
// third of accelerate_shim.hpp (create_fftsetup / destroy_fftsetup /
// fft_zrip). See the header for the full seam contract.
//
// Compiled into: every shim-consuming target on non-Apple platforms, and ONLY
// tests/test_accelerate_shim.cpp on macOS (where the shipped plugins keep
// using real Accelerate and never link pffft).
//
// zrip packing/scaling recap (what call sites bake in):
//   forward:  out = 2 * DFT(x), packed as realp[0]=2*X[0] (DC),
//             imagp[0]=2*X[N/2] (Nyquist), (realp[k],imagp[k])=2*X[k]
//             for k in 1..N/2-1.
//   inverse:  unnormalized Hermitian IDFT of the packed spectrum, i.e.
//             t[n] = S[0] + (-1)^n*S[N/2] + 2*sum_k Re(S[k] e^{+2πi nk/N});
//             round trip forward→inverse = 2N * x (call sites scale 1/(2N)).
//
// pffft's ordered real transform uses the SAME packed spectrum layout
// ([X0, XN/2, ReX1, ImX1, ...]) and the same unnormalized backward
// (backward(forward(x)) = N*x), so the repack layer reduces to:
//   forward:  interleave split→time, pffft FORWARD, scale by 2 while packing.
//   inverse:  pack split→ordered spectrum, pffft BACKWARD, deinterleave.

#include "accelerate_shim.hpp"

#include "pffft.h"

#include <new>

namespace nablafx {
namespace portable_shim {

namespace {
constexpr int kMinLog2n = 5;   // pffft real transforms need N >= 32
constexpr int kMaxLog2n = 20;  // 1M points — far beyond any use here
}  // namespace

// A setup mirrors vDSP semantics: created for a MAXIMUM size 2^log2n, usable
// for any power-of-two size 2^kMinLog2n..2^log2n. All plans and scratch are
// allocated up front so fft_zrip itself is allocation-free (audio-thread
// safe). Scratch is 16-byte aligned via pffft_aligned_malloc (pffft requires
// SIMD alignment for its input/output/work buffers).
struct FFTSetupRec {
    int          max_log2n = 0;
    PFFFT_Setup* plans[kMaxLog2n + 1] = {};  // index = log2n; null below min
    float*       time = nullptr;   // interleaved time-domain scratch (N floats)
    float*       spec = nullptr;   // ordered-spectrum scratch (N floats)
    float*       work = nullptr;   // pffft work area (N floats)
};

FFTSetupRec* create_fftsetup(Length log2n, int radix) {
    if (radix != kRadix2) return nullptr;          // only radix-2 in this repo
    if (log2n < kMinLog2n || log2n > kMaxLog2n) return nullptr;

    FFTSetupRec* s = new (std::nothrow) FFTSetupRec;
    if (!s) return nullptr;
    s->max_log2n = static_cast<int>(log2n);

    const size_t max_n = size_t(1) << log2n;
    s->time = static_cast<float*>(pffft_aligned_malloc(max_n * sizeof(float)));
    s->spec = static_cast<float*>(pffft_aligned_malloc(max_n * sizeof(float)));
    s->work = static_cast<float*>(pffft_aligned_malloc(max_n * sizeof(float)));

    bool ok = s->time && s->spec && s->work;
    for (int l = kMinLog2n; ok && l <= s->max_log2n; ++l) {
        s->plans[l] = pffft_new_setup(1 << l, PFFFT_REAL);
        ok = ok && s->plans[l] != nullptr;
    }
    if (!ok) {
        destroy_fftsetup(s);
        return nullptr;
    }
    return s;
}

void destroy_fftsetup(FFTSetupRec* s) {
    if (!s) return;
    for (int l = kMinLog2n; l <= kMaxLog2n; ++l)
        if (s->plans[l]) pffft_destroy_setup(s->plans[l]);
    pffft_aligned_free(s->time);
    pffft_aligned_free(s->spec);
    pffft_aligned_free(s->work);
    delete s;
}

void fft_zrip(FFTSetupRec* s, const SplitComplex* c, Stride stride,
              Length log2n, int direction) {
    assert(s && c && c->realp && c->imagp);
    assert(stride == 1 && "portable fft_zrip implements stride 1 only");
    (void)stride;
    assert(static_cast<int>(log2n) >= kMinLog2n &&
           static_cast<int>(log2n) <= s->max_log2n);

    const int    n    = 1 << static_cast<int>(log2n);
    const int    half = n / 2;
    PFFFT_Setup* plan = s->plans[log2n];
    assert(plan);

    if (direction == kDirectionForward) {
        // Split (even/odd packing from ctoz) → interleaved time signal.
        for (int i = 0; i < half; ++i) {
            s->time[2 * i]     = c->realp[i];
            s->time[2 * i + 1] = c->imagp[i];
        }
        pffft_transform_ordered(plan, s->time, s->spec, s->work,
                                PFFFT_FORWARD);
        // Ordered spectrum [X0, XN/2, ReX1, ImX1, ...] → zrip packing, with
        // vDSP's forward 2x scale.
        c->realp[0] = 2.0f * s->spec[0];
        c->imagp[0] = 2.0f * s->spec[1];
        for (int k = 1; k < half; ++k) {
            c->realp[k] = 2.0f * s->spec[2 * k];
            c->imagp[k] = 2.0f * s->spec[2 * k + 1];
        }
    } else {
        assert(direction == kDirectionInverse);
        // zrip packing → ordered spectrum.
        s->spec[0] = c->realp[0];
        s->spec[1] = c->imagp[0];
        for (int k = 1; k < half; ++k) {
            s->spec[2 * k]     = c->realp[k];
            s->spec[2 * k + 1] = c->imagp[k];
        }
        // Unnormalized backward (== vDSP inverse zrip, no extra scale).
        pffft_transform_ordered(plan, s->spec, s->time, s->work,
                                PFFFT_BACKWARD);
        // Interleaved time signal → split even/odd packing (for ztoc).
        for (int i = 0; i < half; ++i) {
            c->realp[i] = s->time[2 * i];
            c->imagp[i] = s->time[2 * i + 1];
        }
    }
}

}  // namespace portable_shim
}  // namespace nablafx
