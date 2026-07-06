// Unit tests for CoherenceDistortion (phase-invariant distortion meter via
// magnitude-squared coherence). assert()-based (-UNDEBUG).
//
//   c++ -O2 -std=c++17 -I src tests/test_coherence_distortion.cpp \
//       -framework Accelerate -o test_coherence_distortion \
//       && ./test_coherence_distortion
//
// What is pinned down, and why the tolerances are what they are:
//
//  * LINEAR ⇒ EXACT FLOOR. For wet == dry, wet == -dry and wet == 0.5*dry the
//    FFT outputs are bit-exactly Y = c*X (negation and power-of-two scaling
//    commute exactly with every IEEE add/mul in the pipeline), so
//    Sxy_im == 0, |Sxy|² == Sxx*Syy bit-identically, γ² == 1 up to the eps
//    guard (eps=1e-20 is below half an ulp of the ~1e4 in-band Sxx*Syy
//    products, so 1-γ² is 0 or ~1e-24). distortion < 1e-5 always ⇒ the
//    10*log10(max(d,1e-5)) = -50 dB path clamps to kFloorDb ⇒ the assertion
//    is EXACT equality with floor_db(), not a tolerance.
//
//  * KNOWN ANSWER, additive noise: wet = dry + independent equal-power noise
//    ⇒ per-bin γ² = SNR/(1+SNR) = 1/2 ⇒ distortion = 1/2 ⇒ -3.01 dB. The
//    estimator bias for γ²=0.5 at Neff ≈ 2/ema_a ≈ 38 frame-averages is
//    (1-γ²)²/Neff ≈ 0.007 and the band average spans ~340 bins, so the
//    measurement sits within a few hundredths of a dB of theory; ±1.5 dB is
//    a >20x margin (and the input is a fixed LCG, so the run is deterministic
//    — the tolerance only has to hold for this one input, ever).
//
//  * KNOWN ANSWER, memoryless nonlinearity (Bussgang): wet = g*sign(dry) with
//    dry white UNIFORM noise ⇒ ρ² = E[x·sign x]²/(σx²·σy²) = (A/2)²/(A²/3)
//    = 3/4 per bin (white in ⇒ flat spectra) ⇒ distortion = 1/4 ⇒ -6.02 dB.
//    Asserted within ±3 dB (same determinism argument).
//
//  * DELAY (the module's raison d'être): wet = dry delayed by D=4 samples is
//    linear, so coherence must stay ≈1 even though the naive residual
//    rms(wet-dry) is ~√2·rms (≈0 dB — useless). The only γ² loss is Hann
//    window misalignment: 1-γ² ≈ D²·Σw'²/Σw² ≈ 16·π²/(2·1024)/384 ≈ 2e-4
//    ⇒ ≈ -37 dB. Asserted ≤ -25 dB (12 dB margin, deterministic input).
//
//  * WARM-UP GUARD is an exact boundary: one frame per kHop=512 samples, the
//    value first computes on frame 8 ⇒ sample 4096 exactly.
//
//  * GATE: frames whose windowed DRY energy < 1e-7 must not touch the EMAs ⇒
//    all-zero / denormal-scale / wet-only input never produces a value, and
//    silence after signal HOLDS the last dB bit-exactly.
//
//  * Chunk-size invariance, reset(), re-prepare() and determinism are all
//    bit-exact assertions (the state machine is per-sample).

#include "../src/coherence_distortion.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using nablafx::CoherenceDistortion;

namespace {

constexpr double kSR   = 48000.0;
constexpr int    kHop  = 512;
constexpr int    kLen  = kHop * 192;  // ≈2.05 s, a MULTIPLE OF THE HOP so the
                                      // silence-gate test can reason exactly
                                      // about which post-signal frames still
                                      // see signal in the analysis ring.

// Deterministic LCG white noise, uniform in [-amp, amp). No <random>: the
// distributions there are implementation-defined; this is bit-stable anywhere.
std::vector<float> noise(int n, uint32_t seed, float amp = 0.4f) {
    std::vector<float> o(n);
    uint32_t s = seed ? seed : 1u;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        o[i] = 2.f * amp * ((float)(s >> 8) * (1.0f / 16777216.0f) - 0.5f);
    }
    return o;
}

void push_all(CoherenceDistortion& cd, const std::vector<float>& dry,
              const std::vector<float>& wet, int chunk = 128) {
    assert(dry.size() == wet.size());
    const int n = (int)dry.size();
    for (int i = 0; i < n; i += chunk) {
        const int m = (n - i < chunk) ? (n - i) : chunk;
        cd.push(dry.data() + i, wet.data() + i, m);
    }
}

// Every scenario must keep the output finite and inside [floor_db, 0].
void check_bounds(const CoherenceDistortion& cd) {
    const float db = cd.distortion_db();
    assert(std::isfinite(db));
    assert(db >= CoherenceDistortion::floor_db());
    assert(db <= 0.f);
}

double rms(const float* x, int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / n);
}

// ---------------------------------------------------------------------------
// 1) INITIAL STATE + CONTRACT: floor_db() is the -48 dB the plugin publishes
//    as the idle value; before any signal has_value()==false and
//    distortion_db() pins to the floor. A zero-length push is a no-op.
// ---------------------------------------------------------------------------
void test_initial_state() {
    assert(CoherenceDistortion::floor_db() == -48.f);

    CoherenceDistortion cd;
    cd.prepare(kSR);
    assert(!cd.has_value());
    assert(cd.distortion_db() == CoherenceDistortion::floor_db());
    check_bounds(cd);

    const float dummy = 0.f;
    cd.push(&dummy, &dummy, 0);  // n=0: must not run a frame or move state
    assert(!cd.has_value());
    assert(cd.distortion_db() == CoherenceDistortion::floor_db());
    std::printf("1  initial state + n=0 push            OK\n");
}

// ---------------------------------------------------------------------------
// 2) WARM-UP GUARD, exact boundary: frames fire every 512 samples and the
//    value first computes on frame 8 ⇒ after 4095 samples there is no value,
//    after 4096 there is.
// ---------------------------------------------------------------------------
void test_warmup_boundary() {
    const auto x = noise(8 * kHop, 11);
    CoherenceDistortion cd;
    cd.prepare(kSR);

    std::vector<float> head(x.begin(), x.begin() + 8 * kHop - 1);
    push_all(cd, head, head, 128);   // 4095 samples → 7 frames
    assert(!cd.has_value());
    assert(cd.distortion_db() == CoherenceDistortion::floor_db());

    const float last = x.back();
    cd.push(&last, &last, 1);        // sample 4096 → 8th frame → first value
    assert(cd.has_value());
    // Identity input: the very first value is already the exact floor.
    assert(cd.distortion_db() == CoherenceDistortion::floor_db());
    check_bounds(cd);
    std::printf("2  warm-up boundary (4095/4096)        OK\n");
}

// ---------------------------------------------------------------------------
// 3) LINEAR ⇒ EXACT FLOOR: identity, polarity inversion, and 0.5x gain are
//    all linear maps; coherence must read the exact floor even though the
//    naive residual for the polarity flip is ~2x the signal rms.
// ---------------------------------------------------------------------------
void test_linear_exact_floor() {
    const auto x = noise(kLen, 21);

    // (a) identity
    {
        CoherenceDistortion cd;
        cd.prepare(kSR);
        push_all(cd, x, x);
        assert(cd.has_value());
        assert(cd.distortion_db() == CoherenceDistortion::floor_db());
    }
    // (b) polarity inversion — the archetypal "phase rotation" that breaks a
    //     rms(wet-dry) residual meter but is perfectly linear.
    {
        std::vector<float> y(kLen);
        std::vector<float> resid(kLen);
        for (int i = 0; i < kLen; ++i) { y[i] = -x[i]; resid[i] = y[i] - x[i]; }
        CoherenceDistortion cd;
        cd.prepare(kSR);
        push_all(cd, x, y);
        assert(cd.has_value());
        assert(cd.distortion_db() == CoherenceDistortion::floor_db());
        std::printf("   polarity: naive resid rms=%.3f vs sig rms=%.3f, coherence db=%.1f\n",
                    rms(resid.data(), kLen), rms(x.data(), kLen),
                    (double)cd.distortion_db());
    }
    // (c) exact gain 0.5 (power of two ⇒ bit-exact linear scaling end-to-end)
    {
        std::vector<float> y(kLen);
        for (int i = 0; i < kLen; ++i) y[i] = 0.5f * x[i];
        CoherenceDistortion cd;
        cd.prepare(kSR);
        push_all(cd, x, y);
        assert(cd.has_value());
        assert(cd.distortion_db() == CoherenceDistortion::floor_db());
    }
    std::printf("3  linear maps read exact floor        OK\n");
}

// ---------------------------------------------------------------------------
// 4) DELAY stays coherent: wet = dry >> 4 samples is linear; only Hann
//    misalignment leaks incoherence (theory ≈ -37 dB, see header). Must be
//    far below the nonlinear readings and is asserted ≤ -25 dB.
// ---------------------------------------------------------------------------
float test_delay_coherent() {
    const auto x = noise(kLen, 31);
    const int  D = 4;
    std::vector<float> y(kLen, 0.f);
    for (int i = D; i < kLen; ++i) y[i] = x[i - D];

    CoherenceDistortion cd;
    cd.prepare(kSR);
    push_all(cd, x, y);
    assert(cd.has_value());
    check_bounds(cd);
    const float db = cd.distortion_db();
    std::printf("4  delay(4)  db=%7.2f  (theory ~-37, assert <= -25)\n", (double)db);
    assert(db <= -25.f);
    return db;
}

// ---------------------------------------------------------------------------
// 5) KNOWN ANSWER: wet = dry + independent equal-power noise ⇒ γ² = 1/2 ⇒
//    distortion = 1/2 ⇒ -3.01 dB. Assert ±1.5 dB (deterministic input;
//    derivation in the header puts the true spread at ~0.05 dB).
// ---------------------------------------------------------------------------
float test_known_answer_snr_mix() {
    const auto x = noise(kLen, 41);
    const auto n = noise(kLen, 42);  // independent, same variance
    std::vector<float> y(kLen);
    for (int i = 0; i < kLen; ++i) y[i] = x[i] + n[i];

    CoherenceDistortion cd;
    cd.prepare(kSR);
    push_all(cd, x, y);
    assert(cd.has_value());
    check_bounds(cd);
    const float db = cd.distortion_db();
    std::printf("5  x+noise   db=%7.2f  (theory -3.01, assert in [-4.5,-1.7])\n", (double)db);
    assert(db > -4.5f && db < -1.7f);
    return db;
}

// ---------------------------------------------------------------------------
// 6) KNOWN ANSWER (Bussgang): wet = 0.25*sign(dry), dry uniform white ⇒
//    γ² = 3/4 ⇒ distortion = 1/4 ⇒ -6.02 dB. Assert ±3 dB.
// ---------------------------------------------------------------------------
float test_known_answer_hard_limit() {
    const auto x = noise(kLen, 51);
    std::vector<float> y(kLen);
    for (int i = 0; i < kLen; ++i) y[i] = (x[i] >= 0.f) ? 0.25f : -0.25f;

    CoherenceDistortion cd;
    cd.prepare(kSR);
    push_all(cd, x, y);
    assert(cd.has_value());
    check_bounds(cd);
    const float db = cd.distortion_db();
    std::printf("6  sign(x)   db=%7.2f  (theory -6.02, assert in [-9.0,-3.5])\n", (double)db);
    assert(db > -9.f && db < -3.5f);
    return db;
}

// ---------------------------------------------------------------------------
// 7) UNCORRELATED wet ⇒ near 0 dB: expected γ² ≈ 1/Neff ≈ 0.03 ⇒ ≈ -0.1 dB.
//    Assert > -3 dB (γ̂² would have to exceed 0.5, ~20x the bias) and ≤ 0.
// ---------------------------------------------------------------------------
float test_uncorrelated_near_zero() {
    const auto x = noise(kLen, 61);
    const auto y = noise(kLen, 62);

    CoherenceDistortion cd;
    cd.prepare(kSR);
    push_all(cd, x, y);
    assert(cd.has_value());
    check_bounds(cd);
    const float db = cd.distortion_db();
    std::printf("7  uncorr    db=%7.2f  (theory ~-0.1, assert > -3)\n", (double)db);
    assert(db > -3.f);
    return db;
}

// ---------------------------------------------------------------------------
// 8) SILENCE GATE:
//    a) all-zero input never produces a value;
//    b) dry-silent + wet-loud never produces a value (the gate keys on the
//       DRY energy — the model INPUT is the activity reference);
//    c) denormal-scale input (1e-38) is gated, no NaN/Inf;
//    d) after real signal, silence HOLDS the last dB bit-exactly once the
//       analysis ring has flushed (signal length is a hop multiple, so ring
//       is all-zero after 1024 zero samples ⇒ every later frame is gated).
// ---------------------------------------------------------------------------
void test_silence_gate_and_hold() {
    // (a) zeros
    {
        const std::vector<float> z(kLen, 0.f);
        CoherenceDistortion cd;
        cd.prepare(kSR);
        push_all(cd, z, z);
        assert(!cd.has_value());
        assert(cd.distortion_db() == CoherenceDistortion::floor_db());
    }
    // (b) dry silent, wet loud
    {
        const std::vector<float> z(kLen, 0.f);
        const auto w = noise(kLen, 71);
        CoherenceDistortion cd;
        cd.prepare(kSR);
        push_all(cd, z, w);
        assert(!cd.has_value());
        assert(cd.distortion_db() == CoherenceDistortion::floor_db());
    }
    // (c) denormal-scale: windowed sum-sq ≈ (1e-38)²·384 ≪ 1e-7 ⇒ gated
    {
        const auto t = noise(kLen, 72, 1e-38f);
        CoherenceDistortion cd;
        cd.prepare(kSR);
        push_all(cd, t, t);
        assert(!cd.has_value());
        assert(cd.distortion_db() == CoherenceDistortion::floor_db());
        check_bounds(cd);
    }
    // (d) hold across silence
    {
        const auto x = noise(kLen, 41);
        const auto n = noise(kLen, 42);
        std::vector<float> y(kLen);
        for (int i = 0; i < kLen; ++i) y[i] = x[i] + n[i];

        CoherenceDistortion cd;
        cd.prepare(kSR);
        push_all(cd, x, y);
        // Flush the ring: 2 hops of zeros. kLen % kHop == 0, so after these
        // the ring is entirely zero and every further frame gates out.
        const std::vector<float> flush(2 * kHop, 0.f);
        push_all(cd, flush, flush);
        assert(cd.has_value());
        const float held = cd.distortion_db();

        const std::vector<float> silence(48000, 0.f);  // ~1 s, ~93 gated frames
        push_all(cd, silence, silence);
        assert(cd.has_value());
        assert(cd.distortion_db() == held);  // bit-exact hold
    }
    std::printf("8  silence gate + bit-exact hold       OK\n");
}

// ---------------------------------------------------------------------------
// 9) CHUNK-SIZE INVARIANCE + DETERMINISM: the state machine is per-sample, so
//    pushing the same signal 1-at-a-time, in 128s, or all at once must give
//    bit-identical readings across independent instances.
// ---------------------------------------------------------------------------
void test_chunk_invariance() {
    const auto x = noise(kLen, 41);
    const auto n = noise(kLen, 42);
    std::vector<float> y(kLen);
    for (int i = 0; i < kLen; ++i) y[i] = x[i] + n[i];

    float db[3];
    const int chunks[3] = {1, 128, kLen};
    for (int c = 0; c < 3; ++c) {
        CoherenceDistortion cd;
        cd.prepare(kSR);
        push_all(cd, x, y, chunks[c]);
        assert(cd.has_value());
        db[c] = cd.distortion_db();
    }
    assert(db[0] == db[1]);
    assert(db[1] == db[2]);
    std::printf("9  chunk sizes 1/128/all identical     OK (db=%.4f)\n", (double)db[0]);
}

// ---------------------------------------------------------------------------
// 10) RESET SEMANTICS: reset() returns to the no-value floor state, re-arms
//     the warm-up guard, and a rerun of the same signal reproduces the value
//     bit-exactly (clean slate — no leakage from the previous run).
// ---------------------------------------------------------------------------
void test_reset_semantics() {
    const auto x = noise(kLen, 51);
    std::vector<float> y(kLen);
    for (int i = 0; i < kLen; ++i) y[i] = (x[i] >= 0.f) ? 0.25f : -0.25f;

    CoherenceDistortion cd;
    cd.prepare(kSR);
    push_all(cd, x, y);
    assert(cd.has_value());
    const float first = cd.distortion_db();

    cd.reset();
    assert(!cd.has_value());
    assert(cd.distortion_db() == CoherenceDistortion::floor_db());

    // Warm-up guard is re-armed: 7 frames of signal → still no value.
    std::vector<float> head(x.begin(), x.begin() + 8 * kHop - 1);
    std::vector<float> headw(y.begin(), y.begin() + 8 * kHop - 1);
    push_all(cd, head, headw);
    assert(!cd.has_value());

    cd.reset();
    push_all(cd, x, y);  // full rerun after reset
    assert(cd.has_value());
    assert(cd.distortion_db() == first);  // bit-exact reproduction
    std::printf("10 reset re-arms + reproduces bit-exact OK\n");
}

// ---------------------------------------------------------------------------
// 11) RE-PREPARE at a different sample rate: allocations are redone, state is
//     cleared, and the analytic answers still hold at 44.1 kHz (γ² theory is
//     sample-rate independent; only the EMA constant and band bins move).
// ---------------------------------------------------------------------------
void test_reprepare_44k() {
    CoherenceDistortion cd;
    cd.prepare(kSR);
    const auto x = noise(kLen, 81);
    push_all(cd, x, x);
    assert(cd.has_value());

    cd.prepare(44100.0);  // re-prepare must reset everything
    assert(!cd.has_value());
    assert(cd.distortion_db() == CoherenceDistortion::floor_db());

    push_all(cd, x, x);   // identity at 44.1k → exact floor again
    assert(cd.has_value());
    assert(cd.distortion_db() == CoherenceDistortion::floor_db());

    const auto n = noise(kLen, 82);
    std::vector<float> y(kLen);
    for (int i = 0; i < kLen; ++i) y[i] = x[i] + n[i];
    cd.reset();
    push_all(cd, x, y);   // equal-power mix at 44.1k → same -3.01 dB theory
    assert(cd.has_value());
    check_bounds(cd);
    const float db = cd.distortion_db();
    assert(db > -4.5f && db < -1.7f);
    std::printf("11 re-prepare 44.1k (db=%.2f)          OK\n", (double)db);
}

}  // namespace

int main() {
    test_initial_state();
    test_warmup_boundary();
    test_linear_exact_floor();
    const float db_delay  = test_delay_coherent();
    const float db_mix    = test_known_answer_snr_mix();
    const float db_clip   = test_known_answer_hard_limit();
    const float db_uncorr = test_uncorrelated_near_zero();

    // Severity ordering: linear-ish < clipped < mixed-with-noise < uncorrelated.
    assert(CoherenceDistortion::floor_db() < db_delay);
    assert(db_delay < db_clip);
    assert(db_clip < db_mix);
    assert(db_mix < db_uncorr);

    test_silence_gate_and_hold();
    test_chunk_invariance();
    test_reset_semantics();
    test_reprepare_44k();

    std::printf("test_coherence_distortion: all checks passed\n");
    return 0;
}
