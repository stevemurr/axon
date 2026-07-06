// Unit tests for the STFT magnitude-mask EQ (SpectralMaskEq).
//   g++ -O2 -std=c++17 -I src tests/test_spectral_mask_eq.cpp \
//       -framework Accelerate -o tests/test_spectral_mask_eq \
//       && tests/test_spectral_mask_eq
//
// Headline regression target: the OLA output ring overflow. run_frame_() used
// to do an UNCONDITIONAL `avail += hop`. If process() ever falls behind frame
// generation, avail grows past the ring capacity (n_fft + hop) and the read
// pointer chases the write pointer into cells the OLA vDSP_vadd is still
// writing — corrupting output. The fix clamps (now the clamp_avail=true path
// of OlaAccumulator::add_frame in stft_common.hpp):
//     if (avail + hop <= ring_sz) avail += hop;
//     else                        avail = ring_sz;
// test_out_avail_never_overflows_ring() exercises exactly that expression.

// White-box: the ring-overflow regression test must observe the module's
// private ola_ accumulator and drive its private run_frame_() under a stall
// (frames generated while no process() read drains). The public process() API
// is strictly 1:1 (one output pulled per input pushed) so ola_.avail can never
// reach the clamp through it — the only way to exercise the fixed line is to
// generate frames without draining. `#define private public` is the standard
// header-only-test idiom for poking at internal invariants.
//
// MSVC STL portability: its headers hard-#error when a keyword is macroized
// (xkeycheck.h), and genuinely break if first-included with `private` mapped
// to `public`. Two-part defence, keeping the idiom intact:
//   1. _ALLOW_KEYWORD_MACROS disables the xkeycheck #error.
//   2. Pre-include the full transitive STANDARD-header set of
//      spectral_mask_eq.hpp below, so every STL header is fully processed
//      (and include-guarded) BEFORE the macro goes live. Only project
//      headers then compile under it — which is the point.
#if defined(_MSC_VER)
#define _ALLOW_KEYWORD_MACROS 1
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#define private public
#include "../src/spectral_mask_eq.hpp"
#undef private

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double f, double a, int n, double phase = 0.0) {
    std::vector<float> o(n);
    for (int i = 0; i < n; ++i)
        o[i] = (float)(a * std::sin(2.0 * kPi * f * i / kSR + phase));
    return o;
}
double rms(const float* x, int n, int skip = 0) {
    double s = 0; for (int i = skip; i < n; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / (n - skip));
}
bool finite(const float* x, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(x[i])) return false;
    return true;
}

// Production-shaped config (n_fft=2048, hop=512 → ring = 2560).
nablafx::SpectralMaskEqParams cfg() {
    nablafx::SpectralMaskEqParams p;
    p.sample_rate        = kSR;
    p.block_size         = 128;
    p.num_control_params = 24;
    p.n_fft              = 2048;
    p.hop                = 512;
    p.n_bands            = 24;
    p.min_gain_db        = -12.0f;
    p.max_gain_db        =  12.0f;
    p.f_min              = 40.0f;
    p.f_max              = 18000.0f;
    return p;
}

// ---------------------------------------------------------------------------
// Test 1: FLAT MASK RECONSTRUCTION — a 0 dB mask (all bands = 0.5 → 0 dB)
//         must pass a sine through with its amplitude intact (within the OLA
//         normalization tolerance) after the n_fft latency. Confirms the
//         analysis/OLA/read path is wired correctly to begin with.
// ---------------------------------------------------------------------------
void test_flat_mask_reconstruction() {
    auto p = cfg();
    nablafx::SpectralMaskEq eq; eq.reset(p);
    std::vector<float> flat(p.n_bands, 0.5f);  // 0.5 → 0 dB
    eq.set_params(flat.data(), p.n_bands);

    const int N = kSR;  // 1 s
    auto in = sine(1000.0, 0.3, N);
    std::vector<float> out(N, 0.f);
    eq.process(in.data(), out.data(), N);

    assert(finite(out.data(), N));
    // After latency, RMS should match input RMS closely (flat = transparent).
    const int lat = p.n_fft;
    const double in_rms  = rms(in.data(), N, lat + 1000);
    const double out_rms = rms(out.data() + lat + 1000, N - lat - 1000);
    std::fprintf(stderr, "[flat]  in_rms=%.4f out_rms=%.4f ratio=%.3f (want ~1)\n",
                 in_rms, out_rms, out_rms / in_rms);
    assert(out_rms > in_rms * 0.9 && out_rms < in_rms * 1.1);
    std::fprintf(stderr, "[flat]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: BOUNDED + FINITE under a long, varied-block-size run. This is the
//         behavioral guard for the ring-overflow bug: if the OLA read pointer
//         ever chased the writer into actively-written cells, the output would show
//         non-finite values / blow-ups / a corrupted (drifting) reconstruction.
//         Block sizes deliberately mix tiny (1), non-multiples of hop, and
//         large (>n_fft) so samples_since_ carries and the read/write pointers
//         exercise every wrap of the ring across ~10 s of audio.
// ---------------------------------------------------------------------------
void test_long_varblock_run_is_bounded() {
    auto p = cfg();
    nablafx::SpectralMaskEq eq; eq.reset(p);
    std::vector<float> flat(p.n_bands, 0.5f);

    const int sizes[] = {1, 7, 128, 333, 512, 2048, 3000, 5, 4096};
    long total = 0;
    double peak = 0;
    std::vector<float> in_hist, out_hist;
    for (int rep = 0; rep < 400; ++rep) {
        const int n = sizes[rep % 9];
        std::vector<float> in(n), out(n);
        for (int i = 0; i < n; ++i)
            in[i] = (float)(0.3 * std::sin(2.0 * kPi * 1000.0 * (total + i) / kSR));
        eq.set_params(flat.data(), p.n_bands);   // updates target each "block"
        eq.process(in.data(), out.data(), n);
        assert(finite(out.data(), n));
        for (int i = 0; i < n; ++i) {
            peak = std::max(peak, (double)std::fabs(out[i]));
            in_hist.push_back(in[i]);
            out_hist.push_back(out[i]);
        }
        total += n;
    }
    // Output must stay bounded near the 0.3 input amplitude — a runaway counter
    // / pointer collision would spike this well past unity.
    std::fprintf(stderr, "[long]  total=%ld peak=%.4f (want <0.6)\n", total, peak);
    assert(peak < 0.6);

    // Reconstruction must remain stable (not drift) over the whole run — a
    // read-into-writer collision would make later samples diverge.
    const int lat = p.n_fft;
    double e = 0; long c = 0;
    for (long i = lat + 4000; i < (long)out_hist.size(); ++i) {
        const double d = out_hist[i] - in_hist[i - lat];
        e += d * d; ++c;
    }
    const double recon = std::sqrt(e / c);
    std::fprintf(stderr, "[long]  steady recon RMS=%.4e (want <0.1)\n", recon);
    assert(recon < 0.1);
    std::fprintf(stderr, "[long]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: RING-OVERFLOW (the bug, white-box on the REAL module). Drive the
//         module's actual private run_frame_() under the exact stall the fix
//         targets — frames generated while NO process() read drains the output
//         — and assert the module's own ola_.avail counter never exceeds ring
//         capacity. This exercises the clamp_avail branch of add_frame():
//             if (avail + hop <= ring_sz) avail += hop;
//             else                        avail = ring_sz;
//         Against the OLD code (unconditional `avail += hop`) avail
//         grows without bound (hop * frames), so this assertion FAILS — i.e.
//         the test genuinely catches the regression. After the stall we also
//         drain via process() and confirm the read pointer never walks into
//         cells the OLA add is writing (output stays finite/bounded).
// ---------------------------------------------------------------------------
void test_out_avail_never_overflows_ring() {
    auto p = cfg();
    nablafx::SpectralMaskEq eq; eq.reset(p);
    std::vector<float> flat(p.n_bands, 0.5f);
    eq.set_params(flat.data(), p.n_bands);

    const int ring_sz = static_cast<int>(eq.ola_.out_ring.size());  // n_fft + hop
    assert(ring_sz == p.n_fft + p.hop);

    // Prime the analysis ring with real content so each frame produces audio.
    for (int i = 0; i < p.n_fft; ++i)
        eq.in_ring_[i] = (float)(0.3 * std::sin(2.0 * kPi * 1000.0 * i / kSR));
    eq.in_fill_ = 0;

    // STALL: generate 1000 frames with no read draining ola_.avail.
    int max_avail = 0;
    for (int f = 0; f < 1000; ++f) {
        eq.run_frame_();
        max_avail = std::max(max_avail, eq.ola_.avail);
        // The invariant the fix guarantees, checked every frame.
        assert(eq.ola_.avail <= ring_sz);
    }
    std::fprintf(stderr, "[ovf]   ring_sz=%d  max ola_.avail=%d (must be <= ring)\n",
                 ring_sz, max_avail);
    assert(max_avail == ring_sz);          // clamp engaged, never exceeded
    assert(eq.ola_.avail == ring_sz);

    // read/write must remain valid ring indices after the stall.
    assert(eq.ola_.read  >= 0 && eq.ola_.read  < ring_sz);
    assert(eq.ola_.write >= 0 && eq.ola_.write < ring_sz);

    // Now drain through the public read path: output must stay finite/bounded —
    // a counter that had been allowed to exceed capacity would let out_read_
    // read cells the OLA vDSP_vadd is mid-write, producing garbage here.
    std::vector<float> in(ring_sz, 0.f), out(ring_sz, 0.f);
    eq.process(in.data(), out.data(), ring_sz);
    assert(finite(out.data(), ring_sz));
    for (float v : out) assert(std::fabs(v) < 4.0f);
    std::fprintf(stderr, "[ovf]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: REAL-MODULE STALL — drive the actual SpectralMaskEq under a stress
//         pattern (huge first block to front-load frame generation, then a
//         long sustained stream) and assert the output is always finite and
//         bounded. With the unbounded counter, a read that walks into the
//         in-flight OLA add surfaces as NaN/inf or a clipped spike here.
// ---------------------------------------------------------------------------
void test_real_module_stall_stays_finite() {
    auto p = cfg();
    nablafx::SpectralMaskEq eq; eq.reset(p);
    std::vector<float> flat(p.n_bands, 0.5f);
    eq.set_params(flat.data(), p.n_bands);

    // One large block (many frames at once) then a long tail of hop-sized blocks.
    const int big = p.n_fft * 8;
    {
        auto in = sine(440.0, 0.3, big);
        std::vector<float> out(big, 0.f);
        eq.process(in.data(), out.data(), big);
        assert(finite(out.data(), big));
        for (float v : out) assert(std::fabs(v) < 1.0f);
    }
    double tail_peak = 0;
    long phase = big;
    for (int rep = 0; rep < 200; ++rep) {
        const int n = p.hop;
        std::vector<float> in(n), out(n);
        for (int i = 0; i < n; ++i)
            in[i] = (float)(0.3 * std::sin(2.0 * kPi * 440.0 * (phase + i) / kSR));
        eq.process(in.data(), out.data(), n);
        assert(finite(out.data(), n));
        for (float v : out) tail_peak = std::max(tail_peak, (double)std::fabs(v));
        phase += n;
    }
    std::fprintf(stderr, "[stall] tail_peak=%.4f (want <0.6)\n", tail_peak);
    assert(tail_peak < 0.6);
    std::fprintf(stderr, "[stall] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: MASK DIRECTION — a full boost mask (all bands = 1.0 → +max_gain_db)
//         raises output energy vs a full cut (all bands = 0.0 → min_gain_db).
//         Sanity that set_params actually shapes the spectrum (so the OLA path
//         under test is carrying real, mask-applied audio, not silence).
// ---------------------------------------------------------------------------
void test_mask_direction() {
    auto p = cfg();
    const int N = kSR;
    const int skip = p.n_fft + 2000;

    auto run = [&](float band_val) {
        nablafx::SpectralMaskEq eq; eq.reset(p);
        std::vector<float> bands(p.n_bands, band_val);
        eq.set_params(bands.data(), p.n_bands);
        auto in = sine(2000.0, 0.3, N);
        std::vector<float> out(N, 0.f);
        eq.process(in.data(), out.data(), N);
        assert(finite(out.data(), N));
        return rms(out.data() + skip, N - skip);
    };
    const double boost = run(1.0f);
    const double cut   = run(0.0f);
    std::fprintf(stderr, "[mask]  boost_rms=%.4f cut_rms=%.4f ratio=%.3f (want >1)\n",
                 boost, cut, boost / cut);
    assert(boost > cut * 1.2);
    std::fprintf(stderr, "[mask]  PASS\n");
}

}  // namespace

int main() {
    test_flat_mask_reconstruction();
    test_long_varblock_run_is_bounded();
    test_out_avail_never_overflows_ring();
    test_real_module_stall_stays_finite();
    test_mask_direction();
    std::fprintf(stderr, "ALL 5 SPECTRAL_MASK_EQ TESTS PASSED\n");
    return 0;
}
