// Unit tests for LoudnessMeter.
//
// Build from native/clap/:
//   g++ -O2 -std=c++17 -I src tests/test_meter.cpp src/meter.cpp \
//       src/lufs_leveler.cpp -o tests/test_meter && tests/test_meter

#include "../src/meter.hpp"
#include "../src/lufs_leveler.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> make_sine(double f, double a, int n) {
    std::vector<float> o(n);
    for (int i = 0; i < n; ++i) o[i] = (float)(a * std::sin(2.0 * kPi * f * i / kSR));
    return o;
}
std::vector<float> make_pink(int n, double amp, uint32_t seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<float> o(n); double acc = 0;
    for (int i = 0; i < n; ++i) { acc = 0.995 * acc + 0.05 * nd(rng); o[i] = (float)acc; }
    double pk = 0; for (float v : o) pk = std::max(pk, std::fabs((double)v));
    if (pk > 1e-9) for (float& v : o) v = (float)(v * amp / pk);
    return o;
}

// ---------------------------------------------------------------------------
// Test 1: absolute LUFS matches the trusted LufsLeveler on the same signal
// ---------------------------------------------------------------------------
void test_lufs_matches_leveler() {
    const int N = 5 * kSR;

    // Mono.
    {
        auto sig = make_pink(N, 0.5, 1);
        nablafx::LoudnessMeter m; m.reset(kSR);
        m.process(sig.data(), nullptr, 1, N);

        nablafx::LufsLeveler lv; lv.reset(kSR, -14.0);
        std::vector<float> tmp(sig);
        lv.process(sig.data(), tmp.data(), N);   // measures input LUFS

        double meter = m.readout().lufs_s;
        double ref   = lv.last_measured_lufs();
        std::fprintf(stderr, "[lufs/mono]   meter=%.2f  leveler=%.2f LUFS\n", meter, ref);
        assert(std::fabs(meter - ref) < 0.5);
    }
    // Stereo (decorrelated channels).
    {
        auto l = make_pink(N, 0.5, 2);
        auto r = make_pink(N, 0.5, 3);
        nablafx::LoudnessMeter m; m.reset(kSR);
        m.process(l.data(), r.data(), 2, N);

        nablafx::LufsLeveler lv; lv.reset(kSR, -14.0);
        std::vector<float> lo(l), ro(r);
        lv.process_linked(l.data(), r.data(), lo.data(), ro.data(), N);

        double meter = m.readout().lufs_s;
        double ref   = lv.last_measured_lufs();
        std::fprintf(stderr, "[lufs/stereo] meter=%.2f  leveler=%.2f LUFS\n", meter, ref);
        assert(std::fabs(meter - ref) < 0.5);
    }
    std::fprintf(stderr, "[lufs]        PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: scaling — ×2 amplitude raises LUFS / RMS / peak by ~6.02 dB
// ---------------------------------------------------------------------------
void test_scaling() {
    const int N = 5 * kSR;
    auto run = [&](double amp) {
        auto s = make_pink(N, amp, 9);
        nablafx::LoudnessMeter m; m.reset(kSR);
        m.process(s.data(), nullptr, 1, N);
        return m.readout();
    };
    auto a = run(0.25);
    auto b = run(0.50);
    std::fprintf(stderr,
        "[scale]       lufs %.2f->%.2f (d=%.2f)  rms %.2f->%.2f (d=%.2f)  peak %.2f->%.2f (d=%.2f)\n",
        a.lufs_s, b.lufs_s, b.lufs_s - a.lufs_s,
        a.rms_db, b.rms_db, b.rms_db - a.rms_db,
        a.peak_db, b.peak_db, b.peak_db - a.peak_db);
    assert(std::fabs((b.lufs_s - a.lufs_s) - 6.02) < 0.2);
    assert(std::fabs((b.rms_db - a.rms_db) - 6.02) < 0.2);
    assert(std::fabs((b.peak_db - a.peak_db) - 6.02) < 0.2);
    std::fprintf(stderr, "[scale]       PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: peak & RMS absolute values on a known sine
// ---------------------------------------------------------------------------
void test_peak_rms_sine() {
    const int N = 2 * kSR;
    const double amp = 0.5;            // -6.02 dBFS peak
    auto s = make_sine(1000.0, amp, N);
    nablafx::LoudnessMeter m; m.reset(kSR);
    m.process(s.data(), nullptr, 1, N);
    auto r = m.readout();

    const double want_peak = 20.0 * std::log10(amp);            // -6.02
    const double want_rms  = 20.0 * std::log10(amp / std::sqrt(2.0)); // -9.03
    std::fprintf(stderr, "[sine]        peak=%.2f (want %.2f)  rms=%.2f (want %.2f)\n",
                r.peak_db, want_peak, r.rms_db, want_rms);
    assert(std::fabs(r.peak_db - want_peak) < 0.1);
    assert(std::fabs(r.rms_db  - want_rms)  < 0.2);
    std::fprintf(stderr, "[sine]        PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: silence reads very low everywhere
// ---------------------------------------------------------------------------
void test_silence() {
    const int N = 4 * kSR;
    std::vector<float> z(N, 0.f);
    nablafx::LoudnessMeter m; m.reset(kSR);
    m.process(z.data(), nullptr, 1, N);
    auto r = m.readout();
    std::fprintf(stderr, "[silence]     lufs=%.1f rms=%.1f peak=%.1f\n",
                r.lufs_s, r.rms_db, r.peak_db);
    assert(r.lufs_s  < -70.f);
    assert(r.rms_db  < -100.f);
    assert(r.peak_db < -100.f);
    std::fprintf(stderr, "[silence]     PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: a -14 LUFS target lands in range (sanity for the mastering use case)
// ---------------------------------------------------------------------------
void test_target_minus14() {
    const int N = 6 * kSR;
    // Use the leveler to drive pink noise to -14 LUFS, then meter its output.
    auto src = make_pink(N, 0.7, 5);
    nablafx::LufsLeveler lv; lv.reset(kSR, -14.0);
    std::vector<float> out(N);
    lv.process(src.data(), out.data(), N);

    nablafx::LoudnessMeter m; m.reset(kSR);
    m.process(out.data(), nullptr, 1, N);
    double lufs = m.readout().lufs_s;
    std::fprintf(stderr, "[target]      leveled-to-14 reads %.2f LUFS\n", lufs);
    assert(std::fabs(lufs - (-14.0)) < 1.5);
    std::fprintf(stderr, "[target]      PASS\n");
}

// ---------------------------------------------------------------------------
// Test 6: zero sample rate must not produce div-by-zero garbage.
//
// Regression for the warp_biquad_to_sr / pole-coeff division-by-zero bugs:
// before the reset() guard, reset(0.0, ...) drove K-weighting coeffs (and the
// attack/release pole coeffs) to inf/NaN via src_sr/dst_sr and exp(-1/(tau*fs)),
// so process() emitted NaN/Inf. With the guard a non-positive rate is replaced
// by 48000, keeping every output sample finite.
//
// This also exercises the ring_blocks_ >= 1 path: a zero sample rate would not
// itself zero the ring (ring is short_term_s driven), but the modulo-0 case is
// covered by test 7 below.
// ---------------------------------------------------------------------------
static bool all_finite(const std::vector<float>& v) {
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

void test_zero_samplerate_finite() {
    const int N = 4 * kSR;
    auto src = make_pink(N, 0.5, 11);

    // Reference: the same signal through a leveler that was given the rate the
    // guard substitutes (48000). The zero-rate leveler must behave identically.
    double ref_lufs;
    {
        nablafx::LufsLeveler lv; lv.reset(48000.0, -14.0);
        std::vector<float> out(N, 0.f);
        lv.process(src.data(), out.data(), N);
        ref_lufs = lv.last_measured_lufs();
    }

    // Mono path through a zero-sample-rate leveler.
    {
        nablafx::LufsLeveler lv; lv.reset(0.0, -14.0);   // would div-by-zero pre-fix
        std::vector<float> out(N, 0.f);
        lv.process(src.data(), out.data(), N);
        bool fin = all_finite(out);
        double lufs = lv.last_measured_lufs();
        std::fprintf(stderr, "[zsr/mono]    finite=%d  last_lufs=%.2f (ref %.2f)\n",
                     (int)fin, lufs, ref_lufs);
        assert(fin);                       // catches inf/NaN coeffs from warp
        assert(std::isfinite(lufs));       // catches NaN propagating into LUFS
        // The inf warp coeffs (scale = src_sr/0) corrupt the K-weighting and
        // skew the measured LUFS far from the correctly-initialized reference.
        // A tight match proves the guard ran reset() at a sane 48 kHz instead.
        assert(std::fabs(lufs - ref_lufs) < 0.5);
    }
    // Stereo (linked) path through a zero-sample-rate leveler.
    {
        auto r = make_pink(N, 0.5, 12);
        nablafx::LufsLeveler lv; lv.reset(0.0, -14.0);
        std::vector<float> lo(N, 0.f), ro(N, 0.f);
        lv.process_linked(src.data(), r.data(), lo.data(), ro.data(), N);
        bool fin = all_finite(lo) && all_finite(ro);
        std::fprintf(stderr, "[zsr/stereo]  finite=%d\n", (int)fin);
        assert(fin);
    }
    // Negative sample rate must be sanitized identically.
    {
        nablafx::LufsLeveler lv; lv.reset(-44100.0, -14.0);
        std::vector<float> out(N, 0.f);
        lv.process(src.data(), out.data(), N);
        assert(all_finite(out));
    }
    std::fprintf(stderr, "[zsr]         PASS\n");
}

// ---------------------------------------------------------------------------
// Test 7: zero short-term window must not trigger modulo-by-zero.
//
// Regression for the (ring_idx_ + 1) % ring_blocks_ undefined-behavior bug:
// a Config with short_term_s == 0 made ring_blocks_ == 0, so the modulo in
// process()/process_linked() was % 0 (UB — crash or garbage). The reset()
// guard clamps short_term_s to >= 0.1, guaranteeing ring_blocks_ >= 1. A valid
// sample rate is used so this isolates the short-term/ring fix.
// ---------------------------------------------------------------------------
void test_zero_short_term_window() {
    const int N = 4 * kSR;
    auto src = make_pink(N, 0.5, 13);

    nablafx::LufsLeveler::Config cfg;
    cfg.short_term_s = 0.0;                 // would yield ring_blocks_ == 0 pre-fix
    nablafx::LufsLeveler lv(cfg);
    lv.reset(kSR, -14.0);                   // clamps short_term_s -> ring_blocks_ >= 1

    // If ring_blocks_ were 0 this would be a modulo-0 UB on the first sub-block
    // boundary; reaching here at all (with finite output) demonstrates the fix.
    std::vector<float> out(N, 0.f);
    lv.process(src.data(), out.data(), N);
    bool fin = all_finite(out);
    double lufs = lv.last_measured_lufs();
    std::fprintf(stderr, "[zstw]        finite=%d  last_lufs=%.2f\n",
                 (int)fin, lufs);
    assert(fin);
    // A non-trivial LUFS reading proves the ring was sized >= 1 and actually
    // integrated sub-blocks (it advanced past the -120 init floor).
    assert(std::isfinite(lufs));
    assert(lufs > -120.0);

    // Same for the linked path.
    auto r = make_pink(N, 0.5, 14);
    nablafx::LufsLeveler lv2(cfg);
    lv2.reset(kSR, -14.0);
    std::vector<float> lo(N, 0.f), ro(N, 0.f);
    lv2.process_linked(src.data(), r.data(), lo.data(), ro.data(), N);
    assert(all_finite(lo) && all_finite(ro));
    assert(lv2.last_measured_lufs() > -120.0);

    std::fprintf(stderr, "[zstw]        PASS\n");
}

// ---------------------------------------------------------------------------
// Test 8: LoudnessMeter zero sample rate must not poison the K-weighting biquads.
//
// Regression for the div-by-zero in LoudnessMeter::set_k_weighting_() (meter.cpp).
// A sample_rate of 0 misses the exact 44.1/48 kHz branches and falls through to
// the proportional warp, which computes `48000.0 / sr`. With sr == 0 that is
// Inf, so every K-weighting coefficient (pre_/rlb_) became Inf and the very
// first filtered sample produced Inf - Inf = NaN, poisoning lufs_s / lufs_m.
//
// The fix clamps `if (sr < 1.0) sr = 1.0;` at the top of set_k_weighting_(),
// keeping the warp factor (and thus every coefficient) finite.
//
// We sample the readout right at the first sub-block boundary: with sr == 0,
// reset() collapses sub_len_ to 1 sample, so lufs_s updates on the very first
// sample -- before the heavily-warped (but finite) filter rings up. Pre-fix
// this first update is NaN (Inf coeffs); post-fix it is a finite number.
//
// NOTE: this exercises LoudnessMeter directly. The earlier LufsLeveler tests
// do NOT cover this path (removing the meter.cpp guard leaves them all green).
void test_meter_zero_sample_rate_no_nan() {
    nablafx::LoudnessMeter m;
    m.reset(0.0);                       // div-by-zero path in set_k_weighting_

    // One impulse then zeros: enough to push a value through the K-weighting
    // filter and cross the (1-sample) sub-block boundary, but short enough that
    // the warped filter does not ring up and overflow on its own.
    std::vector<float> s(64, 0.f);
    s[0] = 0.5f;
    m.process(s.data(), nullptr, 1, (int)s.size());

    auto r = m.readout();
    std::fprintf(stderr,
        "[meter/sr0]   lufs_s=%g lufs_m=%g rms=%g peak=%g\n",
        r.lufs_s, r.lufs_m, r.rms_db, r.peak_db);

    // The bug manifests as NaN in the K-weighted LUFS outputs.
    assert(!std::isnan(r.lufs_s));     // catches Inf coeffs -> Inf-Inf -> NaN
    assert(!std::isnan(r.lufs_m));
    // The non-K-weighted paths must stay clean too.
    assert(std::isfinite(r.rms_db));
    assert(std::isfinite(r.peak_db));
    std::fprintf(stderr, "[meter/sr0]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 9: the meter.cpp clamp must NOT alter the exact 48 kHz coefficient path.
//
// The guard only rewrites behavior for sr < 1.0; valid rates take the exact
// branch. Cross-check a 48 kHz meter against the trusted LufsLeveler to prove
// the good path is untouched by the fix.
void test_meter_valid_rate_unaffected() {
    const int SR = 48000;
    const int N  = 5 * SR;
    auto sig = make_pink(N, 0.5, 21);

    nablafx::LoudnessMeter m; m.reset((double)SR);
    m.process(sig.data(), nullptr, 1, N);

    nablafx::LufsLeveler lv; lv.reset((double)SR, -14.0);
    std::vector<float> tmp(sig);
    lv.process(sig.data(), tmp.data(), N);

    double meter = m.readout().lufs_s;
    double ref   = lv.last_measured_lufs();
    std::fprintf(stderr, "[meter/48k]   meter=%.2f  leveler=%.2f LUFS\n", meter, ref);
    assert(std::isfinite(meter));
    assert(std::fabs(meter - ref) < 0.5);
    std::fprintf(stderr, "[meter/48k]   PASS\n");
}

}  // namespace

int main() {
    test_lufs_matches_leveler();
    test_scaling();
    test_peak_rms_sine();
    test_silence();
    test_target_minus14();
    test_zero_samplerate_finite();
    test_zero_short_term_window();
    test_meter_zero_sample_rate_no_nan();
    test_meter_valid_rate_unaffected();
    std::fprintf(stderr, "ALL METER TESTS PASSED\n");
    return 0;
}
