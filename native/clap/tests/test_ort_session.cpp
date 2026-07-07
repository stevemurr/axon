// Unit tests for OrtSession (src/ort_session.cpp) — the realtime ONNX wrapper.
//
// Part 1 (tests 1-5): run() input-length validation via the header-only
// nablafx::ort_run_out_len() (src/ort_run_guard.hpp) — the exact function
// run() calls, so the guard tests exercise the REAL guard, not a mirror.
//
// Part 2 (tests 6-11): the REAL OrtSession — constructor, warmup, run(),
// swap_state_buffers(), reset_state() — against two tiny deterministic ONNX
// graphs embedded below as byte arrays (generated once with onnx-python,
// opset 17 / IR 10; see comments on each array). All expected values are
// exact in float32 (small-integer arithmetic), so every assert is ==, no
// tolerances. This part links onnxruntime and compiles src/ort_session.cpp
// into the test binary (that TU is otherwise only built into the plugin
// modules, i.e. it had zero unit-test coverage).
//
//   c++ -O2 -std=c++17 -UNDEBUG -I src -I build/_deps/onnxruntime-src/include \
//       tests/test_ort_session.cpp src/ort_session.cpp \
//       build/_deps/onnxruntime-src/lib/libonnxruntime.dylib \
//       -Wl,-rpath,build/_deps/onnxruntime-src/lib -o test_ort_session \
//       && ./test_ort_session
//
// (CMake: the test_ort_session target needs src/ort_session.cpp,
//  ${ONNXRUNTIME_INCLUDE_DIR} and ${ONNXRUNTIME_LIBRARY}.)
//
// Guard-history notes (tests 1-3): the PREVIOUSLY-BROKEN path
// (input_len < receptive_field) made audio_out_len negative; cast to size_t it
// became a colossal element count that ONNX would use to write past the
// audio_out_ buffer -> heap corruption. The fix — the
// `input_len < meta_.receptive_field` floor that runs before audio_out_len is
// computed — rejects exactly those inputs by throwing. If someone deletes or
// reorders that guard in ort_run_guard.hpp, these tests fail.

#include "../src/meta.hpp"
#include "../src/ort_run_guard.hpp"
#include "../src/ort_session.hpp"
#include "../src/ort_shape.hpp"   // ort_input_batch() — batch-1 controller guard (#24)

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <chrono>       // unique temp names (no mkstemp on Windows)
#include <filesystem>
#else
#include <unistd.h>
#endif
#include <vector>

namespace {

using nablafx::OrtSession;
using nablafx::PluginMeta;
using nablafx::StateSpec;

// THE REAL precondition + length math of OrtSession::run() — the same function
// run() calls (see src/ort_run_guard.hpp and ort_session.cpp). Returns the
// size_t element count run() passes to Ort::Value::CreateTensor for the
// audio_out_ tensor — i.e. the value that, when bogus, drives the heap
// overflow. Guard order in the real function: max-len check, then the
// receptive_field floor (the fix), THEN audio_out_len is computed.
size_t run_validate(const PluginMeta& meta, int max_input_len, int input_len) {
    return nablafx::ort_run_out_len(meta, max_input_len, input_len);
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

// ===========================================================================
// Part 2 — the REAL OrtSession against embedded deterministic ONNX graphs.
// ===========================================================================
//
// Model A ("kat") — full I/O contract, all values exact in float32:
//   inputs : audio_in  float[1,1,L]  (L dynamic)
//            controls  float[1,2]
//            state_in  float[1,1,4]
//   outputs: audio_out float[1,1,L-3]
//            state_out float[1,1,4]
//   audio_out[i] = (x[i]+x[i+1]+x[i+2]+x[i+3]) * (c0+c1) + sum(state_in)
//                  (Conv1d kernel = ones(4), i.e. receptive_field = 4)
//   state_out    = state_in + 1   (each run increments every state element)
//
// Generated with onnx-python (opset 17, ir_version 10 — ORT 1.20.1 accepts
// IR <= 10) and cross-checked against python-onnxruntime:
//   x = 1..6, c = {2,5}, s = 0  ->  audio_out = {70, 98, 126}, state_out = 1.
const unsigned char kAxonTestModelA[] = {
    0x08, 0x0a, 0x12, 0x0d, 0x61, 0x78, 0x6f, 0x6e, 0x5f, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x6b, 0x61,
    0x74, 0x3a, 0xd9, 0x03, 0x0a, 0x2e, 0x0a, 0x08, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x5f, 0x69, 0x6e,
    0x0a, 0x01, 0x57, 0x12, 0x04, 0x63, 0x6f, 0x6e, 0x76, 0x22, 0x04, 0x43, 0x6f, 0x6e, 0x76, 0x2a,
    0x13, 0x0a, 0x0c, 0x6b, 0x65, 0x72, 0x6e, 0x65, 0x6c, 0x5f, 0x73, 0x68, 0x61, 0x70, 0x65, 0x40,
    0x04, 0xa0, 0x01, 0x07, 0x0a, 0x2c, 0x0a, 0x08, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x73,
    0x12, 0x04, 0x67, 0x73, 0x75, 0x6d, 0x22, 0x09, 0x52, 0x65, 0x64, 0x75, 0x63, 0x65, 0x53, 0x75,
    0x6d, 0x2a, 0x0f, 0x0a, 0x08, 0x6b, 0x65, 0x65, 0x70, 0x64, 0x69, 0x6d, 0x73, 0x18, 0x00, 0xa0,
    0x01, 0x02, 0x0a, 0x19, 0x0a, 0x04, 0x63, 0x6f, 0x6e, 0x76, 0x0a, 0x04, 0x67, 0x73, 0x75, 0x6d,
    0x12, 0x06, 0x73, 0x63, 0x61, 0x6c, 0x65, 0x64, 0x22, 0x03, 0x4d, 0x75, 0x6c, 0x0a, 0x2c, 0x0a,
    0x08, 0x73, 0x74, 0x61, 0x74, 0x65, 0x5f, 0x69, 0x6e, 0x12, 0x04, 0x73, 0x73, 0x75, 0x6d, 0x22,
    0x09, 0x52, 0x65, 0x64, 0x75, 0x63, 0x65, 0x53, 0x75, 0x6d, 0x2a, 0x0f, 0x0a, 0x08, 0x6b, 0x65,
    0x65, 0x70, 0x64, 0x69, 0x6d, 0x73, 0x18, 0x00, 0xa0, 0x01, 0x02, 0x0a, 0x1e, 0x0a, 0x06, 0x73,
    0x63, 0x61, 0x6c, 0x65, 0x64, 0x0a, 0x04, 0x73, 0x73, 0x75, 0x6d, 0x12, 0x09, 0x61, 0x75, 0x64,
    0x69, 0x6f, 0x5f, 0x6f, 0x75, 0x74, 0x22, 0x03, 0x41, 0x64, 0x64, 0x0a, 0x20, 0x0a, 0x08, 0x73,
    0x74, 0x61, 0x74, 0x65, 0x5f, 0x69, 0x6e, 0x0a, 0x04, 0x4f, 0x4e, 0x45, 0x53, 0x12, 0x09, 0x73,
    0x74, 0x61, 0x74, 0x65, 0x5f, 0x6f, 0x75, 0x74, 0x22, 0x03, 0x41, 0x64, 0x64, 0x12, 0x0d, 0x61,
    0x78, 0x6f, 0x6e, 0x5f, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x6b, 0x61, 0x74, 0x2a, 0x1d, 0x08, 0x01,
    0x08, 0x01, 0x08, 0x04, 0x10, 0x01, 0x42, 0x01, 0x57, 0x4a, 0x10, 0x00, 0x00, 0x80, 0x3f, 0x00,
    0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x2a, 0x20, 0x08, 0x01, 0x08,
    0x01, 0x08, 0x04, 0x10, 0x01, 0x42, 0x04, 0x4f, 0x4e, 0x45, 0x53, 0x4a, 0x10, 0x00, 0x00, 0x80,
    0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x5a, 0x1f, 0x0a,
    0x08, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x5f, 0x69, 0x6e, 0x12, 0x13, 0x0a, 0x11, 0x08, 0x01, 0x12,
    0x0d, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x03, 0x12, 0x01, 0x4c, 0x5a, 0x1a,
    0x0a, 0x08, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x73, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01,
    0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x5a, 0x1e, 0x0a, 0x08, 0x73, 0x74,
    0x61, 0x74, 0x65, 0x5f, 0x69, 0x6e, 0x12, 0x12, 0x0a, 0x10, 0x08, 0x01, 0x12, 0x0c, 0x0a, 0x02,
    0x08, 0x01, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x04, 0x62, 0x20, 0x0a, 0x09, 0x61, 0x75,
    0x64, 0x69, 0x6f, 0x5f, 0x6f, 0x75, 0x74, 0x12, 0x13, 0x0a, 0x11, 0x08, 0x01, 0x12, 0x0d, 0x0a,
    0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x03, 0x12, 0x01, 0x4d, 0x62, 0x1f, 0x0a, 0x09,
    0x73, 0x74, 0x61, 0x74, 0x65, 0x5f, 0x6f, 0x75, 0x74, 0x12, 0x12, 0x0a, 0x10, 0x08, 0x01, 0x12,
    0x0c, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x04, 0x42, 0x04, 0x0a,
    0x00, 0x10, 0x11,
};
const size_t kAxonTestModelA_len = sizeof(kAxonTestModelA);

// Model B ("gain2") — minimal contract (rf = 1, no controls, no state):
//   audio_out float[1,1,L] = 2 * audio_in.
// Doubling is exact in IEEE float32 for every finite input incl. subnormals.
const unsigned char kAxonTestModelB[] = {
    0x08, 0x0a, 0x12, 0x0f, 0x61, 0x78, 0x6f, 0x6e, 0x5f, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x67, 0x61,
    0x69, 0x6e, 0x32, 0x3a, 0x84, 0x01, 0x0a, 0x1f, 0x0a, 0x08, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x5f,
    0x69, 0x6e, 0x0a, 0x03, 0x54, 0x57, 0x4f, 0x12, 0x09, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x5f, 0x6f,
    0x75, 0x74, 0x22, 0x03, 0x4d, 0x75, 0x6c, 0x12, 0x0f, 0x61, 0x78, 0x6f, 0x6e, 0x5f, 0x74, 0x65,
    0x73, 0x74, 0x5f, 0x67, 0x61, 0x69, 0x6e, 0x32, 0x2a, 0x0d, 0x10, 0x01, 0x42, 0x03, 0x54, 0x57,
    0x4f, 0x4a, 0x04, 0x00, 0x00, 0x00, 0x40, 0x5a, 0x1f, 0x0a, 0x08, 0x61, 0x75, 0x64, 0x69, 0x6f,
    0x5f, 0x69, 0x6e, 0x12, 0x13, 0x0a, 0x11, 0x08, 0x01, 0x12, 0x0d, 0x0a, 0x02, 0x08, 0x01, 0x0a,
    0x02, 0x08, 0x01, 0x0a, 0x03, 0x12, 0x01, 0x4c, 0x62, 0x20, 0x0a, 0x09, 0x61, 0x75, 0x64, 0x69,
    0x6f, 0x5f, 0x6f, 0x75, 0x74, 0x12, 0x13, 0x0a, 0x11, 0x08, 0x01, 0x12, 0x0d, 0x0a, 0x02, 0x08,
    0x01, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x03, 0x12, 0x01, 0x4c, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11,
};
const size_t kAxonTestModelB_len = sizeof(kAxonTestModelB);

// OrtSession's public ctor takes a filesystem path, so materialize the
// embedded bytes as a temp file (removed in the destructor). POSIX uses
// mkstemp; Windows has no mkstemp, so temp_directory_path + clock nonce +
// counter (single-process/single-threaded test — cannot collide in practice).
struct TempModelFile {
    std::string path;
    explicit TempModelFile(const unsigned char* bytes, size_t len) {
#ifdef _WIN32
        static int counter = 0;
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = (std::filesystem::temp_directory_path() /
                ("axon_test_model_" + std::to_string(nonce) + "_" +
                 std::to_string(counter++)))
                   .string();
        FILE* f = std::fopen(path.c_str(), "wb");
#else
        const char* tmpdir = std::getenv("TMPDIR");
        std::string tmpl = std::string(tmpdir ? tmpdir : "/tmp");
        if (!tmpl.empty() && tmpl.back() != '/') tmpl += '/';
        tmpl += "axon_test_model_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = mkstemp(buf.data());
        assert(fd >= 0);
        FILE* f = fdopen(fd, "wb");
        path.assign(buf.data());
#endif
        assert(f != nullptr);
        size_t written = fwrite(bytes, 1, len, f);
        assert(written == len);
        fclose(f);
    }
    // std::remove == unlink for files on POSIX; portable to Windows.
    ~TempModelFile() { std::remove(path.c_str()); }
};

// Meta matching Model A. rf=4, 2 controls, one state tensor of 4 elements.
PluginMeta meta_model_a() {
    PluginMeta m;
    m.receptive_field = 4;
    m.num_controls    = 2;
    StateSpec s;
    s.name  = "state";
    s.shape = {1, 1, 4};
    s.dtype = "float32";
    m.state_tensors.push_back(s);
    return m;
}

// Meta matching Model B. rf=1, non-parametric, stateless.
PluginMeta meta_model_b() {
    PluginMeta m;
    m.receptive_field = 1;
    m.num_controls    = 0;
    return m;
}

constexpr int kBlockA = 8;   // max_block_len for Model A -> max_input_len = 11
constexpr int kBlockB = 16;  // max_block_len for Model B -> max_input_len = 16

// Fill audio_in with x[i] = i+1 for input_len samples and controls = {2, 5},
// run, and return the output pointer. Expected: out[i] = (4i+10)*7 + state_sum.
const float* run_ramp(OrtSession& s, int input_len) {
    float* x = s.audio_in_buffer();
    for (int i = 0; i < input_len; ++i) x[i] = static_cast<float>(i + 1);
    float* c = s.controls_buffer();
    c[0] = 2.0f;
    c[1] = 5.0f;
    return s.run(input_len);
}

void assert_ramp_output(const float* out, int input_len, float state_sum) {
    const int out_len = input_len - 3;  // rf = 4
    for (int i = 0; i < out_len; ++i) {
        const float expect = static_cast<float>(4 * i + 10) * 7.0f + state_sum;
        assert(out[i] == expect);  // exact: small-integer float32 arithmetic
    }
}

// ---------------------------------------------------------------------------
// Test 6: KNOWN-ANSWER + warmup semantics. A fresh session (ctor ran warmup_()
//         then reset_state()) must produce the analytic output with ZERO state
//         contribution — proving warmup can't leak state into the first block.
//         Also: varying input_len on the same session (buffer reuse) and
//         garbage beyond input_len in audio_in_buffer() must not matter.
// ---------------------------------------------------------------------------
void test_kat_and_warmup_isolation(Ort::Env& env) {
    TempModelFile mf(kAxonTestModelA, kAxonTestModelA_len);
    OrtSession s(env, mf.path, meta_model_a(), kBlockA);

    // Poison the WHOLE input buffer (capacity max_input_len = 11) so any read
    // past input_len would corrupt the result.
    float* x = s.audio_in_buffer();
    for (int i = 0; i < 11; ++i) x[i] = 999.0f;

    // First run after construction: state must be exactly zero (warmup reset).
    const float* out = run_ramp(s, 6);
    assert(out[0] == 70.0f && out[1] == 98.0f && out[2] == 126.0f);
    std::fprintf(stderr, "[kat]   L=6 -> {%g, %g, %g} (want {70, 98, 126})\n",
                 out[0], out[1], out[2]);

    // Different lengths on the same session, same expectations (state still 0
    // because we never swapped). L=4 is the minimum legal call (1 sample),
    // L=11 the maximum (max_block_len = 8 samples).
    out = run_ramp(s, 4);
    assert(out[0] == 70.0f);
    out = run_ramp(s, 11);
    assert_ramp_output(out, 11, 0.0f);
    assert(out[7] == 266.0f);  // (4*7+10)*7 — top of the max-length block
    std::fprintf(stderr, "[kat]   L=4 -> 1 sample, L=11 -> 8 samples, all exact\n");

    // Controls are read live from controls_buffer(): zero them -> output is
    // exactly zero (windowsum*0 + state 0), for the SAME audio input.
    float* c = s.controls_buffer();
    c[0] = 0.0f;
    c[1] = 0.0f;
    out = s.run(6);
    for (int i = 0; i < 3; ++i) assert(out[i] == 0.0f);
    std::fprintf(stderr, "[kat]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 7: STATE DOUBLE-BUFFER SEMANTICS through the public API. Model A's
//         state_out = state_in + 1 and audio_out += sum(state_in), so the
//         state the model SEES is directly observable in the audio:
//           - run twice without swap  -> same input bank -> identical output
//           - swap after each run     -> +4 per run (4 state elems x +1)
//           - reset_state()           -> back to +0
// ---------------------------------------------------------------------------
void test_state_swap_and_reset(Ort::Env& env) {
    TempModelFile mf(kAxonTestModelA, kAxonTestModelA_len);
    OrtSession s(env, mf.path, meta_model_a(), kBlockA);

    // Run 1: state bank A = zeros -> +0.
    const float* out = run_ramp(s, 6);
    float r1[3];
    std::memcpy(r1, out, sizeof(r1));
    assert_ramp_output(r1, 6, 0.0f);

    // Run 2 WITHOUT swap: input bank unchanged -> bit-identical output.
    // (Also the determinism check: same session, same inputs, same bits.)
    out = run_ramp(s, 6);
    assert(std::memcmp(r1, out, sizeof(r1)) == 0);
    std::fprintf(stderr, "[state] no swap -> bit-identical rerun\n");

    // Swap: now the bank written by run 2 (all ones) is the input -> +4.
    s.swap_state_buffers();
    out = run_ramp(s, 6);
    assert_ramp_output(out, 6, 4.0f);
    assert(out[0] == 74.0f);

    // Swap again: that run wrote state = 2 into the other bank -> +8.
    s.swap_state_buffers();
    out = run_ramp(s, 6);
    assert_ramp_output(out, 6, 8.0f);
    assert(out[0] == 78.0f);
    std::fprintf(stderr, "[state] swap chain -> +0, +4, +8 exact\n");

    // Reset: both banks zeroed, input bank back to A -> +0 again, and the
    // output matches run 1 bit-for-bit.
    s.reset_state();
    out = run_ramp(s, 6);
    assert(std::memcmp(r1, out, sizeof(r1)) == 0);
    std::fprintf(stderr, "[state] reset_state -> fresh-session output restored\n");
    std::fprintf(stderr, "[state] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 8: run() GUARDS fire on the REAL object (member run(), not just the
//         free function): too-short and too-long input_len throw
//         std::runtime_error, reach no ORT Run(), and leave the session fully
//         usable with its state intact.
// ---------------------------------------------------------------------------
void test_real_run_guards_and_recovery(Ort::Env& env) {
    TempModelFile mf(kAxonTestModelA, kAxonTestModelA_len);
    OrtSession s(env, mf.path, meta_model_a(), kBlockA);  // max_input_len = 11

    for (int bad : {0, 1, 3, 12, 1000}) {
        bool threw = false;
        try {
            (void)s.run(bad);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        std::fprintf(stderr, "[rguard] run(%d) threw=%d (want 1)\n", bad, (int)threw);
        assert(threw);
    }

    // Session is still healthy and state was never advanced by the rejected
    // calls: the next legal run sees state contribution exactly 0.
    const float* out = run_ramp(s, 6);
    assert_ramp_output(out, 6, 0.0f);
    std::fprintf(stderr, "[rguard] PASS (recovered with state untouched)\n");
}

// ---------------------------------------------------------------------------
// Test 9: MINIMAL MODEL (rf=1, no controls, no state): the num_controls==0
//         branch and the zero-state loops. out = 2*x exactly for every length
//         in [1, max_input_len]; swap/reset on empty state are harmless no-ops.
// ---------------------------------------------------------------------------
void test_minimal_model_all_lengths(Ort::Env& env) {
    TempModelFile mf(kAxonTestModelB, kAxonTestModelB_len);
    OrtSession s(env, mf.path, meta_model_b(), kBlockB);  // max_input_len = 16

    for (int len = 1; len <= kBlockB; ++len) {
        float* x = s.audio_in_buffer();
        for (int i = 0; i < len; ++i)
            x[i] = static_cast<float>(i - 8) * 0.25f;  // exact in float32
        const float* out = s.run(len);
        for (int i = 0; i < len; ++i) {
            const float expect = 2.0f * (static_cast<float>(i - 8) * 0.25f);
            assert(out[i] == expect);  // doubling is exact
        }
        // swap/reset with no state tensors must not disturb anything.
        s.swap_state_buffers();
        s.reset_state();
    }
    std::fprintf(stderr, "[min]   L=1..16 -> out == 2*x exact, empty-state swap/reset ok\n");

    // Guards on this geometry too: rf=1 makes 0 the only too-short length.
    for (int bad : {0, kBlockB + 1}) {
        bool threw = false;
        try { (void)s.run(bad); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);
    }
    std::fprintf(stderr, "[min]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 10: DENORMAL-SCALE inputs pass through unmangled. ORT is created by
//          OrtSession without SetSessionDenormalAsZero, so subnormal samples
//          must double exactly (2*x is exact for every finite float32) — a
//          reverb/decay tail at -700 dBFS must not be flushed or corrupted.
// ---------------------------------------------------------------------------
void test_denormal_passthrough(Ort::Env& env) {
    TempModelFile mf(kAxonTestModelB, kAxonTestModelB_len);
    OrtSession s(env, mf.path, meta_model_b(), kBlockB);

    const float tiny[] = {
        1e-40f,                                      // subnormal
        -1e-40f,
        std::numeric_limits<float>::denorm_min(),    // smallest subnormal
        std::numeric_limits<float>::min(),           // smallest normal
        -std::numeric_limits<float>::min(),
        std::numeric_limits<float>::min() / 2.0f,    // subnormal via halving
        0.0f,
        1.0f,
    };
    const int n = static_cast<int>(sizeof(tiny) / sizeof(tiny[0]));
    float* x = s.audio_in_buffer();
    for (int i = 0; i < n; ++i) x[i] = tiny[i];
    const float* out = s.run(n);
    for (int i = 0; i < n; ++i) {
        const float expect = 2.0f * tiny[i];
        std::fprintf(stderr, "[denrm] 2 * %.6e -> %.6e (want %.6e)\n",
                     tiny[i], out[i], expect);
        assert(out[i] == expect);
    }
    std::fprintf(stderr, "[denrm] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 11: CONSTRUCTOR failure mode. A nonexistent model path must surface as
//          a C++ exception (Ort::Exception is a std::exception), not a crash —
//          the plugin's activate() relies on catching this.
// ---------------------------------------------------------------------------
void test_bad_model_path_throws(Ort::Env& env) {
    bool threw = false;
    try {
        OrtSession s(env, "/nonexistent/axon_test_no_such_model.onnx",
                     meta_model_b(), kBlockB);
    } catch (const std::exception& e) {
        threw = true;
        std::fprintf(stderr, "[ctor]  bad path threw: %.80s...\n", e.what());
    }
    assert(threw);
    std::fprintf(stderr, "[ctor]  PASS\n");
}

}  // namespace

// The activate-time batch guard for the auto-EQ controller (#24): a batched
// controller must declare audio_in batch=2. Model A is a BATCH-1 export
// (audio_in [1,1,L]), so the probe the guard uses must report 1 (-> reject);
// a missing input must report -1. Built from the model bytes directly so the
// test needs no file path.
void test_batch_probe_rejects_batch1(Ort::Env& env) {
    Ort::SessionOptions opts;
    Ort::Session sess(env, kAxonTestModelA, kAxonTestModelA_len, opts);
    const int64_t b = nablafx::ort_input_batch(sess, "audio_in");
    std::fprintf(stderr, "[batch] Model A audio_in batch=%lld (want 1 -> guard rejects)\n",
                 (long long)b);
    assert(b == 1);   // guard throws on != 2, so a batch-1 export is caught
    assert(nablafx::ort_input_batch(sess, "does_not_exist") == -1);
    std::fprintf(stderr, "[batch] PASS\n");
}

int main() {
    // Part 1 — guard arithmetic (no ORT needed).
    test_underflow_is_catastrophic_when_unguarded();
    test_guard_rejects_short_inputs();
    test_boundary_exactly_receptive_field();
    test_valid_range_accepted();
    test_max_len_guard_intact();

    // Part 2 — the real OrtSession (compiles src/ort_session.cpp, links ORT).
    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "test_ort_session");
    test_kat_and_warmup_isolation(env);
    test_state_swap_and_reset(env);
    test_real_run_guards_and_recovery(env);
    test_minimal_model_all_lengths(env);
    test_denormal_passthrough(env);
    test_bad_model_path_throws(env);
    test_batch_probe_rejects_batch1(env);

    std::fprintf(stderr, "ALL 12 TESTS PASSED\n");
    return 0;
}
