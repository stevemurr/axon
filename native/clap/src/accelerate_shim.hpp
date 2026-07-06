// accelerate_shim.hpp — the single Accelerate/vDSP portability seam.
//
// Every DSP unit that used to `#include <Accelerate/Accelerate.h>` includes
// this header instead. Call sites are UNCHANGED — they keep using the exact
// vDSP names, types and semantics.
//
//   * On macOS (__APPLE__): this header includes the real Accelerate and
//     nothing else changes. The mac build stays byte-identical BY
//     CONSTRUCTION — the plugin never touches the portable code below.
//   * On other platforms: the same 17-symbol vDSP surface is provided at
//     global scope by thin aliases over nablafx::portable_shim (pffft-backed
//     FFT + plain scalar loops).
//
// nablafx::portable_shim is declared (and, for the loop ops, fully defined)
// UNCONDITIONALLY — including on macOS — so the mac-side equivalence test
// (tests/test_accelerate_shim.cpp) can compare the portable implementation
// against real vDSP on the same machine. The pffft-backed FFT functions are
// defined in accelerate_shim.cpp, which is compiled into:
//   - every target on non-Apple platforms (Phase 1+), and
//   - ONLY test_accelerate_shim on macOS (with libpffft), keeping pffft out
//     of the shipped mac plugins entirely.
//
// The 17-symbol surface (see docs/future/active/windows-linux-builds.md):
//   FFT lifecycle/transform: vDSP_create_fftsetup, vDSP_destroy_fftsetup,
//                            vDSP_fft_zrip, vDSP_ctoz, vDSP_ztoc
//   complex helpers:         vDSP_zvmul, vDSP_zvmags
//   real vector math:        vDSP_vmul, vDSP_vadd, vDSP_vsmul, vDSP_vsma,
//                            vDSP_vclr, vDSP_svesq, vDSP_dotpr
//   vForce:                  vvexpf, vvcosf, vvsinf
//
// The load-bearing subtlety reproduced here is vDSP's zrip PACKED real-FFT
// format and scaling, which multiple call sites manipulate directly:
//   - after a forward zrip: realp[0] = DC, imagp[0] = Nyquist (both real);
//     bins 1..N/2-1 are (realp[k], imagp[k]);
//   - the forward transform is 2x the mathematical DFT, the inverse is the
//     unnormalized Hermitian IDFT, so a forward+inverse round trip scales by
//     2N — call sites bake in the 1/(2N) (e.g. ola_scale_ in mel_limiter /
//     spectral_mask_eq).
//
// Portable-backend caveat (differs from vDSP, irrelevant to this repo's
// usage): a portable FFTSetup owns per-setup scratch buffers, so it must not
// be used from two threads CONCURRENTLY. Every unit here owns its setup and
// transforms on one thread at a time.

#pragma once

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

#include <cassert>
#include <cmath>
#include <cstddef>

// <Accelerate/Accelerate.h> guarantees M_PI on mac; be explicit elsewhere
// (MSVC's <cmath> needs _USE_MATH_DEFINES, and strict C++ doesn't promise it).
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace nablafx {
namespace portable_shim {

// ---------------------------------------------------------------------------
// vDSP-compatible types (layout-identical to Accelerate's).
// ---------------------------------------------------------------------------
typedef unsigned long Length;  // vDSP_Length
typedef long          Stride;  // vDSP_Stride

struct SplitComplex {  // DSPSplitComplex
    float* realp;
    float* imagp;
};

struct Complex {  // DSPComplex
    float real;
    float imag;
};

enum {
    kRadix2 = 0,  // kFFTRadix2
};

enum {
    kDirectionForward = +1,  // kFFTDirection_Forward / FFT_FORWARD
    kDirectionInverse = -1,  // kFFTDirection_Inverse / FFT_INVERSE
};

// Opaque FFT setup (holds pffft plans + aligned scratch; accelerate_shim.cpp).
struct FFTSetupRec;

// ---------------------------------------------------------------------------
// FFT lifecycle/transform — pffft-backed, defined in accelerate_shim.cpp.
// ---------------------------------------------------------------------------

// vDSP_create_fftsetup: supports power-of-two real FFTs of size 2^5..2^log2n
// (pffft's real-transform minimum is N=32; every size in this repo is
// 1024/2048/4096). Returns nullptr on failure, like vDSP.
FFTSetupRec* create_fftsetup(Length log2n, int radix);
void         destroy_fftsetup(FFTSetupRec* setup);

// vDSP_fft_zrip: in-place real FFT on split-complex data in vDSP's packed
// format, forward = 2x mathematical DFT, inverse = unnormalized Hermitian
// IDFT (round trip = 2N). Only stride 1 is implemented (every call site in
// this repo uses stride 1); asserts otherwise.
void fft_zrip(FFTSetupRec* setup, const SplitComplex* c, Stride stride,
              Length log2n, int direction);

// ---------------------------------------------------------------------------
// Interleave/deinterleave + vector ops — plain loops, defined inline so they
// are compiled into every including TU on every platform.
// Semantics are the documented vDSP definitions, verbatim.
// ---------------------------------------------------------------------------

// vDSP_ctoz: Z->realp[n*IZ] = C[n*IC/2].real; Z->imagp[n*IZ] = C[n*IC/2].imag
// (IC is in FLOATS, must be a multiple of 2 — vDSP's own contract).
inline void ctoz(const Complex* c, Stride ic, const SplitComplex* z,
                 Stride iz, Length n) {
    for (Length i = 0; i < n; ++i) {
        z->realp[i * iz] = c[(i * ic) / 2].real;
        z->imagp[i * iz] = c[(i * ic) / 2].imag;
    }
}

// vDSP_ztoc: C[n*IC/2].real = Z->realp[n*IZ]; C[n*IC/2].imag = Z->imagp[n*IZ]
inline void ztoc(const SplitComplex* z, Stride iz, Complex* c, Stride ic,
                 Length n) {
    for (Length i = 0; i < n; ++i) {
        c[(i * ic) / 2].real = z->realp[i * iz];
        c[(i * ic) / 2].imag = z->imagp[i * iz];
    }
}

// vDSP_zvmul: C = A*B elementwise (conjugate == +1), or conj(A)*B
// (conjugate == -1). Safe for in-place use (any of A/B aliasing C).
inline void zvmul(const SplitComplex* a, Stride ia, const SplitComplex* b,
                  Stride ib, const SplitComplex* c, Stride ic, Length n,
                  int conjugate) {
    for (Length i = 0; i < n; ++i) {
        const float ar = a->realp[i * ia];
        const float ai = (conjugate == -1) ? -a->imagp[i * ia]
                                           : a->imagp[i * ia];
        const float br = b->realp[i * ib];
        const float bi = b->imagp[i * ib];
        const float re = ar * br - ai * bi;
        const float im = ar * bi + ai * br;
        c->realp[i * ic] = re;
        c->imagp[i * ic] = im;
    }
}

// vDSP_zvmags: C[n] = realp[n]^2 + imagp[n]^2
inline void zvmags(const SplitComplex* a, Stride ia, float* c, Stride ic,
                   Length n) {
    for (Length i = 0; i < n; ++i) {
        const float re = a->realp[i * ia];
        const float im = a->imagp[i * ia];
        c[i * ic] = re * re + im * im;
    }
}

// vDSP_vmul: C[n] = A[n] * B[n]
inline void vmul(const float* a, Stride ia, const float* b, Stride ib,
                 float* c, Stride ic, Length n) {
    for (Length i = 0; i < n; ++i) c[i * ic] = a[i * ia] * b[i * ib];
}

// vDSP_vadd: C[n] = A[n] + B[n]
inline void vadd(const float* a, Stride ia, const float* b, Stride ib,
                 float* c, Stride ic, Length n) {
    for (Length i = 0; i < n; ++i) c[i * ic] = a[i * ia] + b[i * ib];
}

// vDSP_vsmul: C[n] = A[n] * B[0]   (B is a pointer to a scalar)
inline void vsmul(const float* a, Stride ia, const float* b, float* c,
                  Stride ic, Length n) {
    const float s = *b;
    for (Length i = 0; i < n; ++i) c[i * ic] = a[i * ia] * s;
}

// vDSP_vsma: D[n] = A[n]*B[0] + C[n]
inline void vsma(const float* a, Stride ia, const float* b, const float* c,
                 Stride ic, float* d, Stride id, Length n) {
    const float s = *b;
    for (Length i = 0; i < n; ++i) d[i * id] = a[i * ia] * s + c[i * ic];
}

// vDSP_vclr: C[n] = 0
inline void vclr(float* c, Stride ic, Length n) {
    for (Length i = 0; i < n; ++i) c[i * ic] = 0.0f;
}

// vDSP_svesq: C[0] = sum A[n]^2 (sequential accumulation; vDSP's SIMD
// partial sums may differ in the last ulps — the equivalence test bounds it).
inline void svesq(const float* a, Stride ia, float* c, Length n) {
    float acc = 0.0f;
    for (Length i = 0; i < n; ++i) acc += a[i * ia] * a[i * ia];
    *c = acc;
}

// vDSP_dotpr: C[0] = sum A[n]*B[n]
inline void dotpr(const float* a, Stride ia, const float* b, Stride ib,
                  float* c, Length n) {
    float acc = 0.0f;
    for (Length i = 0; i < n; ++i) acc += a[i * ia] * b[i * ib];
    *c = acc;
}

// vForce equivalents: y[i] = f(x[i]) via std::. libm results may differ from
// Apple vForce in the last ulp(s); the equivalence test bounds this too.
inline void vv_expf(float* y, const float* x, const int* n) {
    for (int i = 0; i < *n; ++i) y[i] = std::exp(x[i]);
}
inline void vv_cosf(float* y, const float* x, const int* n) {
    for (int i = 0; i < *n; ++i) y[i] = std::cos(x[i]);
}
inline void vv_sinf(float* y, const float* x, const int* n) {
    for (int i = 0; i < *n; ++i) y[i] = std::sin(x[i]);
}

}  // namespace portable_shim
}  // namespace nablafx

#ifndef __APPLE__
// ---------------------------------------------------------------------------
// Non-Apple: expose the vDSP names/types at global scope, exactly as
// Accelerate does, backed by portable_shim. Call sites compile unmodified.
// ---------------------------------------------------------------------------

typedef nablafx::portable_shim::Length        vDSP_Length;
typedef nablafx::portable_shim::Stride        vDSP_Stride;
typedef nablafx::portable_shim::SplitComplex  DSPSplitComplex;
typedef nablafx::portable_shim::Complex       DSPComplex;
typedef nablafx::portable_shim::FFTSetupRec*  FFTSetup;
typedef int                                   FFTDirection;
typedef int                                   FFTRadix;

enum {
    kFFTRadix2 = nablafx::portable_shim::kRadix2,
};
enum {
    kFFTDirection_Forward = nablafx::portable_shim::kDirectionForward,
    kFFTDirection_Inverse = nablafx::portable_shim::kDirectionInverse,
};

inline FFTSetup vDSP_create_fftsetup(vDSP_Length log2n, FFTRadix radix) {
    return nablafx::portable_shim::create_fftsetup(log2n, radix);
}
inline void vDSP_destroy_fftsetup(FFTSetup setup) {
    nablafx::portable_shim::destroy_fftsetup(setup);
}
inline void vDSP_fft_zrip(FFTSetup setup, const DSPSplitComplex* c,
                          vDSP_Stride ic, vDSP_Length log2n,
                          FFTDirection direction) {
    nablafx::portable_shim::fft_zrip(setup, c, ic, log2n, direction);
}
inline void vDSP_ctoz(const DSPComplex* c, vDSP_Stride ic,
                      const DSPSplitComplex* z, vDSP_Stride iz, vDSP_Length n) {
    nablafx::portable_shim::ctoz(c, ic, z, iz, n);
}
inline void vDSP_ztoc(const DSPSplitComplex* z, vDSP_Stride iz, DSPComplex* c,
                      vDSP_Stride ic, vDSP_Length n) {
    nablafx::portable_shim::ztoc(z, iz, c, ic, n);
}
inline void vDSP_zvmul(const DSPSplitComplex* a, vDSP_Stride ia,
                       const DSPSplitComplex* b, vDSP_Stride ib,
                       const DSPSplitComplex* c, vDSP_Stride ic, vDSP_Length n,
                       int conjugate) {
    nablafx::portable_shim::zvmul(a, ia, b, ib, c, ic, n, conjugate);
}
inline void vDSP_zvmags(const DSPSplitComplex* a, vDSP_Stride ia, float* c,
                        vDSP_Stride ic, vDSP_Length n) {
    nablafx::portable_shim::zvmags(a, ia, c, ic, n);
}
inline void vDSP_vmul(const float* a, vDSP_Stride ia, const float* b,
                      vDSP_Stride ib, float* c, vDSP_Stride ic, vDSP_Length n) {
    nablafx::portable_shim::vmul(a, ia, b, ib, c, ic, n);
}
inline void vDSP_vadd(const float* a, vDSP_Stride ia, const float* b,
                      vDSP_Stride ib, float* c, vDSP_Stride ic, vDSP_Length n) {
    nablafx::portable_shim::vadd(a, ia, b, ib, c, ic, n);
}
inline void vDSP_vsmul(const float* a, vDSP_Stride ia, const float* b,
                       float* c, vDSP_Stride ic, vDSP_Length n) {
    nablafx::portable_shim::vsmul(a, ia, b, c, ic, n);
}
inline void vDSP_vsma(const float* a, vDSP_Stride ia, const float* b,
                      const float* c, vDSP_Stride ic, float* d, vDSP_Stride id,
                      vDSP_Length n) {
    nablafx::portable_shim::vsma(a, ia, b, c, ic, d, id, n);
}
inline void vDSP_vclr(float* c, vDSP_Stride ic, vDSP_Length n) {
    nablafx::portable_shim::vclr(c, ic, n);
}
inline void vDSP_svesq(const float* a, vDSP_Stride ia, float* c,
                       vDSP_Length n) {
    nablafx::portable_shim::svesq(a, ia, c, n);
}
inline void vDSP_dotpr(const float* a, vDSP_Stride ia, const float* b,
                       vDSP_Stride ib, float* c, vDSP_Length n) {
    nablafx::portable_shim::dotpr(a, ia, b, ib, c, n);
}
inline void vvexpf(float* y, const float* x, const int* n) {
    nablafx::portable_shim::vv_expf(y, x, n);
}
inline void vvcosf(float* y, const float* x, const int* n) {
    nablafx::portable_shim::vv_cosf(y, x, n);
}
inline void vvsinf(float* y, const float* x, const int* n) {
    nablafx::portable_shim::vv_sinf(y, x, n);
}

#endif  // !__APPLE__
