// test_accelerate_shim — equivalence net for the Accelerate portability seam
// (src/accelerate_shim.hpp / .cpp). This is the regression oracle for the
// whole Windows/Linux port: it proves, on macOS, that the portable
// (pffft-backed) implementation in nablafx::portable_shim reproduces real
// vDSP's zrip packed format, scaling and vector-op semantics — so a non-mac
// build computes the same DSP the mac null-test discipline already guards.
//
// Sections:
//   1. Known-answer packing pins, per backend: DC lands in realp[0], Nyquist
//      in imagp[0], forward = 2x the mathematical DFT.
//   2. Forward zrip, portable vs vDSP: N in {1024, 2048, 4096} x
//      {impulse@0, impulse@3, DC, Nyquist sine, mixed sines, seeded noise},
//      packed layout bin-for-bin within 1e-4 relative (measured bound is
//      printed; typically ~1e-7 — see PHASE-0 NOTES below).
//   3. Inverse round-trip through EACH backend: reconstructs the input with
//      the vDSP 1/(2N) convention the call sites bake in.
//   4. Every vector op vs an in-test scalar reference: EXACT (bitwise).
//   5. Vector ops vs real vDSP: elementwise ops exact; fma-able and
//      reduction ops within tiny documented tolerances.
//   6. vForce (vvexpf/vvcosf/vvsinf) vs std:: loops within ulp-level bounds.
//
// On non-Apple platforms the global vDSP names ARE the portable shim, so the
// cross-backend sections become self-comparisons (trivially green) while the
// known-answer, round-trip and scalar-reference sections stay load-bearing.
//
// PHASE-0 MEASURED BOUNDS (macOS arm64, Apple clang -O2, 2026-07-06):
//   forward zrip portable-vs-vDSP: 2.384e-07 max rel (bound 1e-4)
//   round-trip vs input:           4.774e-07 max rel (bound 1e-4)
//   vForce vs std::                2.0 ulp max        (bound 4 ulp / 5e-7)
// The asserted bounds stay at 1e-4 deliberately — they are the acceptance
// bar from the phase brief, ~400x above the measured noise floor, so the
// test is robust across compilers/SIMD paths without masking real breakage.

#include "../src/accelerate_shim.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace ps = nablafx::portable_shim;

// ---------------------------------------------------------------- helpers

static double g_max_fwd_rel = 0.0;        // measured: fwd portable vs vDSP
static double g_max_rt_rel = 0.0;         // measured: round-trip vs input
static double g_max_vforce_ulp = 0.0;     // measured: vForce vs std

static int ilog2(int n) {
    int l = 0;
    while ((1 << l) < n) ++l;
    assert((1 << l) == n);
    return l;
}

static double ulp_diff(float a, float b) {
    if (a == b) return 0.0;
    if (std::isnan(a) || std::isnan(b)) return 1e30;
    int32_t ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    // Map to a monotonic integer line (two's-complement trick).
    if (ia < 0) ia = INT32_MIN - ia;
    if (ib < 0) ib = INT32_MIN - ib;
    return std::abs((double)ia - (double)ib);
}

// Forward zrip through the PUBLIC vDSP names (real Accelerate on macOS).
static void fwd_vdsp(const std::vector<float>& x, std::vector<float>& re,
                     std::vector<float>& im) {
    const int n = (int)x.size();
    const vDSP_Length log2n = (vDSP_Length)ilog2(n);
    FFTSetup setup = vDSP_create_fftsetup(log2n, kFFTRadix2);
    assert(setup);
    re.assign(n / 2, 0.0f);
    im.assign(n / 2, 0.0f);
    DSPSplitComplex sp{re.data(), im.data()};
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(x.data()), 2, &sp, 1,
              (vDSP_Length)(n / 2));
    vDSP_fft_zrip(setup, &sp, 1, log2n, kFFTDirection_Forward);
    vDSP_destroy_fftsetup(setup);
}

// Forward zrip through the portable namespace (pffft-backed everywhere).
static void fwd_portable(const std::vector<float>& x, std::vector<float>& re,
                         std::vector<float>& im) {
    const int n = (int)x.size();
    const ps::Length log2n = (ps::Length)ilog2(n);
    ps::FFTSetupRec* setup = ps::create_fftsetup(log2n, ps::kRadix2);
    assert(setup);
    re.assign(n / 2, 0.0f);
    im.assign(n / 2, 0.0f);
    ps::SplitComplex sp{re.data(), im.data()};
    ps::ctoz(reinterpret_cast<const ps::Complex*>(x.data()), 2, &sp, 1,
             (ps::Length)(n / 2));
    ps::fft_zrip(setup, &sp, 1, log2n, ps::kDirectionForward);
    ps::destroy_fftsetup(setup);
}

// Forward + inverse + 1/(2N) through the vDSP names; returns reconstruction.
static std::vector<float> roundtrip_vdsp(const std::vector<float>& x) {
    const int n = (int)x.size();
    const vDSP_Length log2n = (vDSP_Length)ilog2(n);
    FFTSetup setup = vDSP_create_fftsetup(log2n, kFFTRadix2);
    assert(setup);
    std::vector<float> re(n / 2), im(n / 2), out(n);
    DSPSplitComplex sp{re.data(), im.data()};
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(x.data()), 2, &sp, 1,
              (vDSP_Length)(n / 2));
    vDSP_fft_zrip(setup, &sp, 1, log2n, kFFTDirection_Forward);
    vDSP_fft_zrip(setup, &sp, 1, log2n, kFFTDirection_Inverse);
    vDSP_ztoc(&sp, 1, reinterpret_cast<DSPComplex*>(out.data()), 2,
              (vDSP_Length)(n / 2));
    const float scale = 1.0f / (2.0f * (float)n);  // the call sites' 1/(2N)
    for (auto& v : out) v *= scale;
    vDSP_destroy_fftsetup(setup);
    return out;
}

static std::vector<float> roundtrip_portable(const std::vector<float>& x) {
    const int n = (int)x.size();
    const ps::Length log2n = (ps::Length)ilog2(n);
    ps::FFTSetupRec* setup = ps::create_fftsetup(log2n, ps::kRadix2);
    assert(setup);
    std::vector<float> re(n / 2), im(n / 2), out(n);
    ps::SplitComplex sp{re.data(), im.data()};
    ps::ctoz(reinterpret_cast<const ps::Complex*>(x.data()), 2, &sp, 1,
             (ps::Length)(n / 2));
    ps::fft_zrip(setup, &sp, 1, log2n, ps::kDirectionForward);
    ps::fft_zrip(setup, &sp, 1, log2n, ps::kDirectionInverse);
    ps::ztoc(&sp, 1, reinterpret_cast<ps::Complex*>(out.data()), 2,
             (ps::Length)(n / 2));
    const float scale = 1.0f / (2.0f * (float)n);
    for (auto& v : out) v *= scale;
    ps::destroy_fftsetup(setup);
    return out;
}

// The five signal families from the phase brief (+ an off-zero impulse so
// nontrivial phase hits every bin).
static std::vector<std::pair<const char*, std::vector<float>>>
make_signals(int n) {
    std::vector<std::pair<const char*, std::vector<float>>> sigs;

    std::vector<float> imp0(n, 0.0f);
    imp0[0] = 1.0f;
    sigs.emplace_back("impulse@0", std::move(imp0));

    std::vector<float> imp3(n, 0.0f);
    imp3[3] = 1.0f;
    sigs.emplace_back("impulse@3", std::move(imp3));

    sigs.emplace_back("DC", std::vector<float>(n, 1.0f));

    std::vector<float> nyq(n);
    for (int i = 0; i < n; ++i) nyq[i] = (i & 1) ? -1.0f : 1.0f;
    sigs.emplace_back("nyquist", std::move(nyq));

    std::vector<float> mix(n);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / n;
        mix[i] = (float)(std::sin(2.0 * M_PI * 5.0 * t) +
                         0.5 * std::sin(2.0 * M_PI * 37.7 * t + 1.3) +
                         0.25 * std::sin(2.0 * M_PI * (n / 4 + 0.5) * t));
    }
    sigs.emplace_back("mixed-sines", std::move(mix));

    std::mt19937 rng(12345u + (unsigned)n);
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    std::vector<float> noise(n);
    for (auto& v : noise) v = uni(rng);
    sigs.emplace_back("noise", std::move(noise));

    return sigs;
}

// ------------------------------------------------- 1. packing known-answers

// Pins the zrip conventions per backend, independent of each other:
// DC in realp[0], Nyquist in imagp[0], forward = 2x mathematical DFT.
static void test_packing_kat() {
    const int n = 1024;
    const float tol = 1e-2f;  // absolute, against components of size 2n=2048

    for (int backend = 0; backend < 2; ++backend) {
        auto fwd = backend == 0 ? fwd_vdsp : fwd_portable;
        std::vector<float> re, im;

        // DC (all ones): X[0] = N -> realp[0] = 2N; everything else ~0.
        fwd(std::vector<float>(n, 1.0f), re, im);
        assert(std::fabs(re[0] - 2.0f * n) <= tol);
        assert(std::fabs(im[0]) <= tol);  // Nyquist bin of DC signal = 0
        for (int k = 1; k < n / 2; ++k) {
            assert(std::fabs(re[k]) <= tol);
            assert(std::fabs(im[k]) <= tol);
        }

        // Nyquist-rate sine (+1,-1,...): X[N/2] = N -> imagp[0] = 2N.
        std::vector<float> nyq(n);
        for (int i = 0; i < n; ++i) nyq[i] = (i & 1) ? -1.0f : 1.0f;
        fwd(nyq, re, im);
        assert(std::fabs(im[0] - 2.0f * n) <= tol);
        assert(std::fabs(re[0]) <= tol);
        for (int k = 1; k < n / 2; ++k) {
            assert(std::fabs(re[k]) <= tol);
            assert(std::fabs(im[k]) <= tol);
        }

        // Impulse at 0: X[k] = 1 for all k -> realp[k] = 2, imagp[0] = 2.
        std::vector<float> imp(n, 0.0f);
        imp[0] = 1.0f;
        fwd(imp, re, im);
        assert(std::fabs(re[0] - 2.0f) <= 1e-4f);
        assert(std::fabs(im[0] - 2.0f) <= 1e-4f);
        for (int k = 1; k < n / 2; ++k) {
            assert(std::fabs(re[k] - 2.0f) <= 1e-4f);
            assert(std::fabs(im[k]) <= 1e-4f);
        }
    }
    std::printf("[shim] packing KATs (DC->realp[0], Nyquist->imagp[0], 2x fwd "
                "scale): OK on both backends\n");
}

// ------------------------------------------- 2+3. fwd equivalence/round-trip

static void test_fft_equivalence() {
    for (int n : {1024, 2048, 4096}) {
        for (auto& [name, x] : make_signals(n)) {
            std::vector<float> re_v, im_v, re_p, im_p;
            fwd_vdsp(x, re_v, im_v);
            fwd_portable(x, re_p, im_p);

            // Bin-for-bin on the PACKED layout, relative to the spectrum's
            // peak magnitude component (pure per-bin relative error is
            // ill-defined at ~0 bins, e.g. every non-DC bin of the DC
            // signal).
            float peak = 0.0f;
            for (int k = 0; k < n / 2; ++k) {
                peak = std::max(peak, std::fabs(re_v[k]));
                peak = std::max(peak, std::fabs(im_v[k]));
            }
            assert(peak > 0.0f);
            double max_rel = 0.0;
            for (int k = 0; k < n / 2; ++k) {
                max_rel = std::max(
                    max_rel, (double)std::fabs(re_v[k] - re_p[k]) / peak);
                max_rel = std::max(
                    max_rel, (double)std::fabs(im_v[k] - im_p[k]) / peak);
            }
            g_max_fwd_rel = std::max(g_max_fwd_rel, max_rel);
            if (max_rel > 1e-4) {
                std::printf("[shim] FWD MISMATCH n=%d sig=%s rel=%.3e\n", n,
                            name, max_rel);
                assert(false && "forward zrip portable vs vDSP exceeded 1e-4");
            }

            // Round-trip through each backend with the call sites' 1/(2N).
            float xpeak = 0.0f;
            for (float v : x) xpeak = std::max(xpeak, std::fabs(v));
            for (int backend = 0; backend < 2; ++backend) {
                auto y = backend == 0 ? roundtrip_vdsp(x) : roundtrip_portable(x);
                double max_rt = 0.0;
                for (int i = 0; i < n; ++i)
                    max_rt = std::max(
                        max_rt, (double)std::fabs(y[i] - x[i]) / xpeak);
                g_max_rt_rel = std::max(g_max_rt_rel, max_rt);
                if (max_rt > 1e-4) {
                    std::printf("[shim] ROUND-TRIP MISMATCH n=%d sig=%s "
                                "backend=%s rel=%.3e\n",
                                n, name, backend == 0 ? "vdsp" : "portable",
                                max_rt);
                    assert(false && "round-trip exceeded 1e-4 of input peak");
                }
            }
        }
    }
    std::printf("[shim] fwd zrip portable-vs-vDSP max rel err: %.3e "
                "(bound 1e-4)\n", g_max_fwd_rel);
    std::printf("[shim] round-trip (1/(2N)) max rel err:       %.3e "
                "(bound 1e-4)\n", g_max_rt_rel);
}

// --------------------------------------- 4+5. vector ops: exact + vs vDSP

static void test_vector_ops() {
    std::mt19937 rng(777u);
    std::uniform_real_distribution<float> uni(-2.0f, 2.0f);

    for (int n : {1, 7, 64, 1023, 4096}) {
        std::vector<float> a(n), b(n), c(n), d0(n), d1(n), d2(n);
        for (int i = 0; i < n; ++i) {
            a[i] = uni(rng);
            b[i] = uni(rng);
            c[i] = uni(rng);
        }
        const float s = 0.37f;

        // vmul — portable vs scalar reference: EXACT; vs vDSP: exact
        // (elementwise IEEE multiply).
        ps::vmul(a.data(), 1, b.data(), 1, d0.data(), 1, (ps::Length)n);
        for (int i = 0; i < n; ++i) d1[i] = a[i] * b[i];
        assert(std::memcmp(d0.data(), d1.data(), n * 4) == 0);
        vDSP_vmul(a.data(), 1, b.data(), 1, d2.data(), 1, (vDSP_Length)n);
        assert(std::memcmp(d0.data(), d2.data(), n * 4) == 0);

        // vadd — same expectations.
        ps::vadd(a.data(), 1, b.data(), 1, d0.data(), 1, (ps::Length)n);
        for (int i = 0; i < n; ++i) d1[i] = a[i] + b[i];
        assert(std::memcmp(d0.data(), d1.data(), n * 4) == 0);
        vDSP_vadd(a.data(), 1, b.data(), 1, d2.data(), 1, (vDSP_Length)n);
        assert(std::memcmp(d0.data(), d2.data(), n * 4) == 0);

        // vsmul — exact both ways (single multiply per element).
        ps::vsmul(a.data(), 1, &s, d0.data(), 1, (ps::Length)n);
        for (int i = 0; i < n; ++i) d1[i] = a[i] * s;
        assert(std::memcmp(d0.data(), d1.data(), n * 4) == 0);
        vDSP_vsmul(a.data(), 1, &s, d2.data(), 1, (vDSP_Length)n);
        assert(std::memcmp(d0.data(), d2.data(), n * 4) == 0);

        // vsma (d = a*s + c) — portable vs reference EXACT; vDSP may fuse
        // into fma on arm64. An fma vs mul+add difference is bounded by the
        // rounding of the product, so compare with a magnitude-scaled bound
        // (a fixed ulp bound is wrong near cancellation, where the OUTPUT
        // can be arbitrarily small).
        const float feps = 1.1920929e-7f;  // 2^-23
        ps::vsma(a.data(), 1, &s, c.data(), 1, d0.data(), 1, (ps::Length)n);
        for (int i = 0; i < n; ++i) d1[i] = a[i] * s + c[i];
        assert(std::memcmp(d0.data(), d1.data(), n * 4) == 0);
        vDSP_vsma(a.data(), 1, &s, c.data(), 1, d2.data(), 1, (vDSP_Length)n);
        for (int i = 0; i < n; ++i) {
            const float mag = std::fabs(a[i] * s) + std::fabs(c[i]);
            assert(std::fabs(d0[i] - d2[i]) <= 2.0f * feps * mag);
        }

        // vclr — exact everywhere.
        d0 = a;
        ps::vclr(d0.data(), 1, (ps::Length)n);
        for (float v : d0) assert(v == 0.0f);
        d2 = a;
        vDSP_vclr(d2.data(), 1, (vDSP_Length)n);
        assert(std::memcmp(d0.data(), d2.data(), n * 4) == 0);

        // svesq / dotpr — portable vs reference EXACT (same sequential
        // accumulation); vDSP uses SIMD partial sums, so compare within a
        // relative tolerance.
        float r0, r1, r2;
        ps::svesq(a.data(), 1, &r0, (ps::Length)n);
        r1 = 0.0f;
        for (int i = 0; i < n; ++i) r1 += a[i] * a[i];
        assert(std::memcmp(&r0, &r1, 4) == 0);
        vDSP_svesq(a.data(), 1, &r2, (vDSP_Length)n);
        assert(std::fabs(r0 - r2) <= 1e-5f * std::max(1.0f, std::fabs(r2)));

        ps::dotpr(a.data(), 1, b.data(), 1, &r0, (ps::Length)n);
        r1 = 0.0f;
        for (int i = 0; i < n; ++i) r1 += a[i] * b[i];
        assert(std::memcmp(&r0, &r1, 4) == 0);
        vDSP_dotpr(a.data(), 1, b.data(), 1, &r2, (vDSP_Length)n);
        assert(std::fabs(r0 - r2) <= 1e-5f * std::max(1.0f, std::fabs(r2)));

        // zvmags — portable vs reference EXACT; vDSP may fma, so allow the
        // product-rounding bound (no cancellation here: both terms >= 0).
        if (n >= 2) {
            const int h = n / 2;
            std::vector<float> zr(a.begin(), a.begin() + h);
            std::vector<float> zi(b.begin(), b.begin() + h);
            ps::SplitComplex zp{zr.data(), zi.data()};
            std::vector<float> m0(h), m1(h), m2(h);
            ps::zvmags(&zp, 1, m0.data(), 1, (ps::Length)h);
            for (int i = 0; i < h; ++i) m1[i] = zr[i] * zr[i] + zi[i] * zi[i];
            assert(std::memcmp(m0.data(), m1.data(), h * 4) == 0);
            DSPSplitComplex zv{zr.data(), zi.data()};
            vDSP_zvmags(&zv, 1, m2.data(), 1, (vDSP_Length)h);
            for (int i = 0; i < h; ++i)
                assert(std::fabs(m0[i] - m2[i]) <= 2.0f * feps * m1[i]);

            // zvmul (conjugate = +1, exactly the spectral_mask_eq call
            // shape, in-place on the first operand) — portable vs reference
            // EXACT; vs vDSP within the product-rounding bound (vDSP's
            // fma-fused complex multiply differs most, in RELATIVE terms,
            // where ar*br ~ ai*bi cancel — so the bound must scale with the
            // products, not the output).
            std::vector<float> xr(zr), xi(zi), yr(h), yi(h);
            for (int i = 0; i < h; ++i) {
                yr[i] = uni(rng);
                yi[i] = uni(rng);
            }
            std::vector<float> pr(xr), pi(xi);  // portable copy (in-place)
            ps::SplitComplex px{pr.data(), pi.data()};
            ps::SplitComplex py{yr.data(), yi.data()};
            ps::zvmul(&px, 1, &py, 1, &px, 1, (ps::Length)h, 1);
            for (int i = 0; i < h; ++i) {
                const float rr = xr[i] * yr[i] - xi[i] * yi[i];
                const float ii = xr[i] * yi[i] + xi[i] * yr[i];
                assert(std::memcmp(&pr[i], &rr, 4) == 0);
                assert(std::memcmp(&pi[i], &ii, 4) == 0);
            }
            std::vector<float> vr(xr), vi(xi);  // vDSP copy (in-place)
            DSPSplitComplex vx{vr.data(), vi.data()};
            DSPSplitComplex vy{yr.data(), yi.data()};
            vDSP_zvmul(&vx, 1, &vy, 1, &vx, 1, (vDSP_Length)h, 1);
            for (int i = 0; i < h; ++i) {
                const float mag_re =
                    std::fabs(xr[i] * yr[i]) + std::fabs(xi[i] * yi[i]);
                const float mag_im =
                    std::fabs(xr[i] * yi[i]) + std::fabs(xi[i] * yr[i]);
                assert(std::fabs(pr[i] - vr[i]) <= 2.0f * feps * mag_re);
                assert(std::fabs(pi[i] - vi[i]) <= 2.0f * feps * mag_im);
            }

            // ctoz/ztoc with the call sites' strides (2 / 1) — pure data
            // movement, exact everywhere, and a ztoc(ctoz(x)) identity.
            std::vector<float> inter(2 * h);
            for (int i = 0; i < 2 * h; ++i) inter[i] = uni(rng);
            std::vector<float> sr0(h), si0(h), sr2(h), si2(h);
            ps::SplitComplex sp0{sr0.data(), si0.data()};
            ps::ctoz(reinterpret_cast<const ps::Complex*>(inter.data()), 2,
                     &sp0, 1, (ps::Length)h);
            for (int i = 0; i < h; ++i) {
                assert(sr0[i] == inter[2 * i] && si0[i] == inter[2 * i + 1]);
            }
            DSPSplitComplex sp2{sr2.data(), si2.data()};
            vDSP_ctoz(reinterpret_cast<const DSPComplex*>(inter.data()), 2,
                      &sp2, 1, (vDSP_Length)h);
            assert(std::memcmp(sr0.data(), sr2.data(), h * 4) == 0);
            assert(std::memcmp(si0.data(), si2.data(), h * 4) == 0);

            std::vector<float> back0(2 * h), back2(2 * h);
            ps::ztoc(&sp0, 1, reinterpret_cast<ps::Complex*>(back0.data()), 2,
                     (ps::Length)h);
            assert(std::memcmp(back0.data(), inter.data(), 2 * h * 4) == 0);
            vDSP_ztoc(&sp2, 1, reinterpret_cast<DSPComplex*>(back2.data()), 2,
                      (vDSP_Length)h);
            assert(std::memcmp(back2.data(), inter.data(), 2 * h * 4) == 0);
        }
    }
    std::printf("[shim] vector ops: portable == scalar reference (bitwise); "
                "vs vDSP within documented ulp/rel bounds\n");
}

// ----------------------------------------------------- 6. vForce vs std::

static void test_vforce() {
    const int n = 4096;
    std::vector<float> x(n), y_v(n), y_p(n);

    // exp over the ranges the cepstral path feeds it (log-magnitudes).
    for (int i = 0; i < n; ++i)
        x[i] = -30.0f + 40.0f * (float)i / (float)(n - 1);
    vvexpf(y_v.data(), x.data(), &n);
    ps::vv_expf(y_p.data(), x.data(), &n);
    for (int i = 0; i < n; ++i) {
        const double u = ulp_diff(y_v[i], y_p[i]);
        g_max_vforce_ulp = std::max(g_max_vforce_ulp, u);
        assert(u <= 4.0);
    }

    // cos/sin over several periods (phase angles from the min-phase path).
    for (int i = 0; i < n; ++i)
        x[i] = -8.0f * (float)M_PI +
               16.0f * (float)M_PI * (float)i / (float)(n - 1);
    vvcosf(y_v.data(), x.data(), &n);
    ps::vv_cosf(y_p.data(), x.data(), &n);
    for (int i = 0; i < n; ++i) {
        // cos/sin are bounded by 1; near zeros relative/ulp bounds degrade,
        // so bound |diff| absolutely at the 4-ulp-of-1.0 level.
        assert(std::fabs(y_v[i] - y_p[i]) <= 5e-7f);
        g_max_vforce_ulp = std::max(g_max_vforce_ulp, ulp_diff(y_v[i], y_p[i]));
    }
    vvsinf(y_v.data(), x.data(), &n);
    ps::vv_sinf(y_p.data(), x.data(), &n);
    for (int i = 0; i < n; ++i)
        assert(std::fabs(y_v[i] - y_p[i]) <= 5e-7f);

    std::printf("[shim] vForce vs std: max %.1f ulp (exp bound 4 ulp; "
                "cos/sin bound 5e-7 abs)\n", g_max_vforce_ulp);
}

int main() {
    test_packing_kat();
    test_fft_equivalence();
    test_vector_ops();
    test_vforce();
    std::printf("test_accelerate_shim: ALL OK\n");
    std::printf("  measured: fwd %.3e rel, round-trip %.3e rel, "
                "vForce %.1f ulp\n",
                g_max_fwd_rel, g_max_rt_rel, g_max_vforce_ulp);
    return 0;
}
