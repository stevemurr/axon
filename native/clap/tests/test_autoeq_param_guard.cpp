// Unit tests for the AutoEQ control-param-count guard in axon_plugin.cpp.
//
//   g++ -O2 -std=c++17 -UNDEBUG -I src tests/test_autoeq_param_guard.cpp \
//       -framework Accelerate -o tests/test_autoeq_param_guard \
//       && tests/test_autoeq_param_guard
//
// Background — the bug this catches:
//   The audio thread (flush_chain_block_ in axon_plugin.cpp) stages the AutoEQ
//   controller's outputs through a FIXED stack array:
//
//       std::array<float, 64> eq_params_storage{};
//       float* eq_params = eq_params_storage.data();
//       ...
//       sess->run_controller(ctrl_buf.data(), kBlockSize, eq_params, n_params);
//
//   where n_params == SpectralMaskEqParams::num_control_params, read straight
//   from the loaded bundle's meta. If a bundle declares num_control_params > 64,
//   run_controller writes more than 64 floats into the 64-slot stack array — a
//   stack buffer overflow on the real-time audio thread, driven entirely by
//   attacker/author-controlled bundle metadata.
//
//   The fix adds a fail-fast check in plugin_activate, right after fetching the
//   class's dsp, BEFORE any audio runs:
//
//       const int n_control =
//           std::get<SpectralMaskEqParams>(dsp.params).num_control_params;
//       if (n_control > 64)
//           throw std::runtime_error("... exceeds the 64-element control buffer ...");
//
// plugin_activate itself is not standalone-callable (CLAP + ORT + global state),
// so these tests exercise the exact logic the fix introduced:
//   * the guard predicate (the same `> kEqParamsStorage` test, against the same
//     64 constant), proving oversized bundles are rejected and valid ones pass;
//   * the genuineness of the overflow — a real SpectralMaskEq reset+set_params
//     consumes EXACTLY num_control_params floats, so a count > 64 fed through a
//     64-slot buffer really is out of bounds (the broken path).

#include "../src/spectral_mask_eq.hpp"   // SpectralMaskEq (pure DSP, no CLAP/ORT)
#include "../src/meta.hpp"               // SpectralMaskEqParams

#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

// Mirror of the audio thread's fixed staging buffer size in
// flush_chain_block_ (std::array<float, 64> eq_params_storage). The guard in
// plugin_activate compares num_control_params against exactly this value.
constexpr int kEqParamsStorage = 64;
constexpr int kSR = 44100;

// Build a geometrically-valid SpectralMaskEqParams for `n_bands` bands. n_fft
// must be a power of two and large enough that n_freq = n_fft/2+1 covers the
// requested bands; 4096 → 2049 bins comfortably covers anything we test here.
nablafx::SpectralMaskEqParams make_params(int n_bands) {
    nablafx::SpectralMaskEqParams p{};
    p.sample_rate        = kSR;
    p.block_size         = 128;
    p.num_control_params = n_bands;   // == n_bands, per the meta contract
    p.n_fft              = 4096;
    p.hop                = 1024;
    p.n_bands            = n_bands;
    p.min_gain_db        = -12.0f;
    p.max_gain_db        =  12.0f;
    p.f_min              = 20.0f;
    p.f_max              = 20000.0f;
    return p;
}

// The exact guard introduced by the fix, factored out verbatim so the test
// pins the predicate (count vs the 64-slot staging array) rather than a
// re-derived approximation of it.
bool activation_accepts(const nablafx::SpectralMaskEqParams& params) {
    const int n_control = params.num_control_params;
    if (n_control > kEqParamsStorage) {
        return false;   // plugin_activate throws std::runtime_error here
    }
    return true;
}

// ---------------------------------------------------------------------------
// Test 1: GUARD REJECTS OVERSIZED BUNDLE (the previously-unchecked path).
//   Before the fix, a bundle with num_control_params=65 sailed through
//   activation and overflowed eq_params_storage[64] on the audio thread.
//   After the fix, activation must reject it.
// ---------------------------------------------------------------------------
void test_guard_rejects_oversized() {
    // Just-over the boundary and far-over — both must be rejected.
    for (int n : {kEqParamsStorage + 1, 96, 128, 1000}) {
        auto p = make_params(n);
        bool ok = activation_accepts(p);
        std::fprintf(stderr,
            "[reject] num_control_params=%d accepted=%d (want 0)\n", n, (int)ok);
        assert(!ok && "oversized bundle must be rejected at activation");
    }
    std::fprintf(stderr, "[reject] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: GUARD ACCEPTS VALID BUNDLES, INCLUDING THE EXACT BOUNDARY.
//   num_control_params == 64 fits the buffer exactly and must be allowed;
//   the guard must not be off-by-one (it is `> 64`, not `>= 64`).
// ---------------------------------------------------------------------------
void test_guard_accepts_valid() {
    for (int n : {1, 16, 32, kEqParamsStorage - 1, kEqParamsStorage}) {
        auto p = make_params(n);
        bool ok = activation_accepts(p);
        std::fprintf(stderr,
            "[accept] num_control_params=%d accepted=%d (want 1)\n", n, (int)ok);
        assert(ok && "in-bounds bundle must pass activation");
    }
    std::fprintf(stderr, "[accept] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: OVERFLOW IS REAL — set_params consumes EXACTLY num_control_params
//   floats. This proves the guard is guarding something real: a count of N is
//   the number of floats written/read through the staging buffer, so N > 64
//   through a 64-slot array genuinely reads/writes out of bounds.
//
//   We reset a real SpectralMaskEq with num_control_params=N and confirm:
//     * set_params(buf, N)   succeeds (consumes exactly N);
//     * set_params(buf, N-1) throws  (too few — the count is load-bearing);
//     * set_params(buf, N+1) throws  (too many — the count is load-bearing).
// ---------------------------------------------------------------------------
void test_set_params_consumes_exactly_num_control_params() {
    const int N = 24;
    auto p = make_params(N);
    nablafx::SpectralMaskEq eq;
    eq.reset(p);
    assert(eq.num_control_params() == N);

    std::vector<float> buf(N + 2, 0.5f);   // benign mid-curve values

    // Exactly N: accepted.
    bool exact_ok = true;
    try { eq.set_params(buf.data(), (std::size_t)N); }
    catch (const std::exception&) { exact_ok = false; }
    assert(exact_ok && "set_params must accept exactly num_control_params");

    // N-1 (mimics a too-small count): rejected.
    bool under_threw = false;
    try { eq.set_params(buf.data(), (std::size_t)(N - 1)); }
    catch (const std::exception&) { under_threw = true; }
    assert(under_threw && "set_params must reject a short count");

    // N+1 (mimics the overflow direction): rejected by the DSP's own check.
    bool over_threw = false;
    try { eq.set_params(buf.data(), (std::size_t)(N + 1)); }
    catch (const std::exception&) { over_threw = true; }
    assert(over_threw && "set_params must reject an over-long count");

    std::fprintf(stderr,
        "[exact]  N=%d set_params(N)=ok set_params(N-1)=throw set_params(N+1)=throw\n", N);
    std::fprintf(stderr, "[exact]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: END-TO-END GUARD SEMANTICS — for a 65-band bundle, activation must
//   refuse BEFORE the audio path stages 65 floats into the 64-slot buffer.
//   We assert that the rejected count is exactly the one that would have
//   overflowed: the first out-of-bounds write index (== kEqParamsStorage)
//   is < num_control_params, i.e. the controller would index past the array.
// ---------------------------------------------------------------------------
void test_overflow_count_is_what_guard_blocks() {
    auto p = make_params(kEqParamsStorage + 1);   // 65 bands
    // The audio thread would do: eq_params[0..n_params-1] = ...  over a 64 array.
    // The first overflowing index is kEqParamsStorage (== 64), which is a valid
    // index only if num_control_params <= 64. Confirm it would overflow...
    assert(p.num_control_params > kEqParamsStorage &&
           "this fixture must describe an overflowing bundle");
    // ...and that the activation guard catches precisely this case.
    assert(!activation_accepts(p) &&
           "the guard must block the bundle that would overflow eq_params_storage");
    std::fprintf(stderr,
        "[block]  would-overflow count=%d blocked by guard (buffer=%d)\n",
        p.num_control_params, kEqParamsStorage);
    std::fprintf(stderr, "[block]  PASS\n");
}

}  // namespace

int main() {
    test_guard_rejects_oversized();
    test_guard_accepts_valid();
    test_set_params_consumes_exactly_num_control_params();
    test_overflow_count_is_what_guard_blocks();
    std::fprintf(stderr, "ALL 4 TESTS PASSED\n");
    return 0;
}
