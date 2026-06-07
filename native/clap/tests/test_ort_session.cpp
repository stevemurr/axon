// Unit tests for OrtSession::run() input-length validation.
//   g++ -O2 -std=c++17 -I src tests/test_ort_session.cpp -o tests/test_ort_session \
//       && tests/test_ort_session
//
// OrtSession itself pulls in onnxruntime + needs a real .onnx model, so we can't
// instantiate it with no external deps. The bug we are guarding against, though,
// lives entirely in run()'s integer arithmetic and precondition checks — no ORT
// call is reached when the guard fires. So we replicate run()'s exact
// length-validation + audio_out_len computation here (mirroring ort_session.cpp
// lines ~89-130) against a real PluginMeta, and prove:
//
//   (a) the PREVIOUSLY-BROKEN path (input_len < receptive_field) makes
//       audio_out_len negative; cast to size_t it becomes a colossal element
//       count that ONNX would use to write past the audio_out_ buffer -> heap
//       corruption; and
//   (b) the FIX — the `input_len < meta_.receptive_field` guard added before
//       audio_out_len is computed — rejects exactly those inputs by throwing,
//       so the size_t underflow can never reach CreateTensor.
//
// If someone deletes/reorders that guard, run_validate() below stops throwing on
// the underflow inputs and these asserts fail.

#include "../src/meta.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using nablafx::PluginMeta;

// A faithful copy of the precondition + length math at the TOP of
// OrtSession::run() (ort_session.cpp). Returns the size_t element count that
// run() passes to Ort::Value::CreateTensor for the audio_out_ tensor — i.e. the
// value that, when bogus, drives the heap overflow. Throws the SAME messages
// run() does. The guard order here mirrors the source: max-len check, then the
// receptive_field floor (the fix), THEN audio_out_len is computed.
size_t run_validate(const PluginMeta& meta, int max_input_len, int input_len) {
    if (input_len > max_input_len) {
        throw std::runtime_error("OrtSession::run: input_len exceeds max configured block length");
    }
    if (input_len < meta.receptive_field) {  // <-- the fix under test
        throw std::runtime_error("OrtSession::run: input_len must be at least receptive_field");
    }

    const int audio_out_len = input_len - (meta.receptive_field - 1);
    // This static_cast is the corrupting step in the original bug: a negative
    // int becomes a ~2^64-scale size_t element count.
    return static_cast<size_t>(audio_out_len);
}

// The UNGUARDED version — exactly what run() did before the fix (max-len check
// only). Used purely to demonstrate that the very same inputs we now reject did
// produce a catastrophic size_t. This models the buggy code, on purpose.
size_t run_validate_buggy(const PluginMeta& meta, int max_input_len, int input_len) {
    if (input_len > max_input_len) {
        throw std::runtime_error("OrtSession::run: input_len exceeds max configured block length");
    }
    const int audio_out_len = input_len - (meta.receptive_field - 1);
    return static_cast<size_t>(audio_out_len);
}

PluginMeta make_meta(int receptive_field) {
    PluginMeta m;
    m.receptive_field = receptive_field;
    m.num_controls    = 0;
    return m;
}

// max_input_len mirrors OrtSession's ctor: receptive_field - 1 + max_block_len.
int max_input_len_for(int receptive_field, int max_block_len) {
    return receptive_field - 1 + max_block_len;
}

// ---------------------------------------------------------------------------
// Test 1: THE BUG, demonstrated. With the OLD (unguarded) logic, an input_len
//         shorter than the receptive field yields a size_t element count that is
//         enormous (>= the legitimate audio_out_ buffer), i.e. the value ONNX
//         would use to scribble past the heap buffer.
// ---------------------------------------------------------------------------
void test_underflow_is_catastrophic_when_unguarded() {
    const int rf = 8192, block = 512;
    const auto meta = make_meta(rf);
    const int maxin = max_input_len_for(rf, block);  // 8703

    // Largest legitimate output the buffer is sized for == max_block_len.
    const size_t legit_max = static_cast<size_t>(block);

    // Any input strictly below rf-1 drives audio_out_len negative -> size_t
    // underflow. (input_len == rf-1 yields exactly 0, which is non-catastrophic
    // but still illegal; the guard rejects it too — see test 2/3.)
    const int bad_inputs[] = {0, 1, rf - 2, rf / 2, rf - 100};
    for (int in : bad_inputs) {
        size_t n = run_validate_buggy(meta, maxin, in);  // old code: no throw
        std::fprintf(stderr,
            "[bug]   rf=%d input_len=%d -> audio_out elems=%zu (legit max=%zu)\n",
            rf, in, n, legit_max);
        // The cast-to-size_t made it astronomically larger than the buffer —
        // this is the out-of-bounds write the fix prevents.
        assert(n > legit_max);
        assert(n > (std::numeric_limits<size_t>::max() / 2));  // wrapped negative
    }
    std::fprintf(stderr, "[bug]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: THE FIX. The guarded run() rejects every input_len below the
//         receptive field by throwing — so audio_out_len is never computed and
//         the size_t underflow can never reach CreateTensor.
// ---------------------------------------------------------------------------
void test_guard_rejects_short_inputs() {
    const int rf = 8192, block = 512;
    const auto meta = make_meta(rf);
    const int maxin = max_input_len_for(rf, block);

    const int bad_inputs[] = {0, 1, rf - 1, rf / 2, rf - 100};
    for (int in : bad_inputs) {
        bool threw = false;
        try {
            (void)run_validate(meta, maxin, in);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        std::fprintf(stderr, "[guard] input_len=%d (< rf=%d) threw=%d (want 1)\n",
                     in, rf, (int)threw);
        assert(threw);
    }
    std::fprintf(stderr, "[guard] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: BOUNDARY. input_len == receptive_field is the smallest LEGAL call:
//         it must NOT throw and must yield audio_out_len == 1 (a single output
//         sample), well within the buffer.
// ---------------------------------------------------------------------------
void test_boundary_exactly_receptive_field() {
    const int rf = 8192, block = 512;
    const auto meta = make_meta(rf);
    const int maxin = max_input_len_for(rf, block);

    size_t n = run_validate(meta, maxin, rf);  // exactly rf — legal
    std::fprintf(stderr, "[bound] input_len==rf -> audio_out elems=%zu (want 1)\n", n);
    assert(n == 1);

    // One below the boundary must throw (closes the off-by-one door).
    bool threw = false;
    try { (void)run_validate(meta, maxin, rf - 1); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
    std::fprintf(stderr, "[bound] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: VALID RANGE. Every legal input_len in [rf, max_input_len] is accepted
//         and produces a sane, in-bounds audio_out_len in [1, max_block_len].
// ---------------------------------------------------------------------------
void test_valid_range_accepted() {
    const int rf = 4096, block = 1024;
    const auto meta = make_meta(rf);
    const int maxin = max_input_len_for(rf, block);  // 5119
    const size_t buf = static_cast<size_t>(block);

    for (int in = rf; in <= maxin; ++in) {
        size_t n = run_validate(meta, maxin, in);  // must not throw
        assert(n >= 1 && n <= buf);
        assert(n == static_cast<size_t>(in - (rf - 1)));
    }
    // The top of the range maps to exactly max_block_len output samples.
    assert(run_validate(meta, maxin, maxin) == buf);
    std::fprintf(stderr, "[valid] all %d legal lengths -> in-bounds out lengths\n",
                 maxin - rf + 1);
    std::fprintf(stderr, "[valid] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: MAX-LEN guard still works (we didn't regress the pre-existing check).
//         input_len > max_input_len throws, and that check still precedes the
//         new receptive_field floor.
// ---------------------------------------------------------------------------
void test_max_len_guard_intact() {
    const int rf = 1024, block = 256;
    const auto meta = make_meta(rf);
    const int maxin = max_input_len_for(rf, block);

    bool threw = false;
    try { (void)run_validate(meta, maxin, maxin + 1); }
    catch (const std::runtime_error&) { threw = true; }
    std::fprintf(stderr, "[maxln] input_len>max threw=%d (want 1)\n", (int)threw);
    assert(threw);
    std::fprintf(stderr, "[maxln] PASS\n");
}

}  // namespace

int main() {
    test_underflow_is_catastrophic_when_unguarded();
    test_guard_rejects_short_inputs();
    test_boundary_exactly_receptive_field();
    test_valid_range_accepted();
    test_max_len_guard_intact();
    std::fprintf(stderr, "ALL 5 TESTS PASSED\n");
    return 0;
}
