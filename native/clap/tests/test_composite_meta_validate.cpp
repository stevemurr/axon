// Self-contained validation tests for the composite axon_meta.json loader.
// Unlike test_composite_meta.cpp (which needs a real staged fixture), this
// writes its own temp JSON files so it can exercise malformed/edge inputs.
//
// Regression target: a negative `lookahead_ms` in axon_meta.json used to be
// accepted, producing a negative delay_samples and out-of-bounds ring-buffer
// access in the ceiling limiter. The loader must now THROW on lookahead_ms<0.
//
//   g++ -O2 -std=c++17 -I../src -I<json>/single_include -UNDEBUG \
//       test_composite_meta_validate.cpp ../src/composite_meta.cpp \
//       ../src/meta.cpp -o test_composite_meta_validate

#include "../src/composite_meta.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

// Minimal schema-version-2 axon_meta.json with a parameterizable lookahead_ms.
// Everything the loader does a .at() on must be present, or it would throw for
// the wrong reason.
std::string make_meta_json(const std::string& lookahead_field) {
    return std::string(R"({
  "schema_version": 2,
  "effect_name": "Axon",
  "model_id": "test-model",
  "sample_rate": 44100,
  "channels": 2,
  "sub_bundles": { "saturator": "sat_dir" },
  "auto_eq": {
    "default_class": "vocal",
    "class_order": ["vocal"],
    "classes": { "vocal": "aeq_vocal_dir" }
  },
  "controls": {},
  "amount_mapping": {
    "saturator": { "pre_gain_db_max": 12.0, "post_gain_db_max": -12.0, "wet_mix_max": 1.0 },
    "auto_eq":   { "wet_mix_max": 1.0 }
  },
  "leveler": { "target_lufs": -14.0 },
  "ceiling": {
    "ceiling_dbtp": -1.0)") + lookahead_field + R"(,
    "attack_ms": 0.5,
    "release_ms": 50.0
  }
})";
}

std::string write_temp(const std::string& contents, const char* tag) {
    std::string path = std::string("/tmp/axon_meta_test_") + tag + ".json";
    std::ofstream o(path, std::ios::trunc);
    o << contents;
    o.close();
    return path;
}

// ---------------------------------------------------------------------------
// Test 1: VALID lookahead — a non-negative value loads cleanly and round-trips.
// ---------------------------------------------------------------------------
void test_valid_lookahead_loads() {
    auto path = write_temp(make_meta_json(R"(, "lookahead_ms": 1.5)"), "valid");
    auto m = nablafx::load_composite_meta(path);
    std::fprintf(stderr, "[valid] lookahead_ms=%g (want 1.5)\n", m.ceiling.lookahead_ms);
    assert(m.ceiling.lookahead_ms == 1.5f);
    assert(m.ceiling.ceiling_dbtp == -1.0f);
    std::fprintf(stderr, "[valid] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: ZERO lookahead — the boundary value (>=0) must be ACCEPTED. A
//         too-strict `<= 0` guard would wrongly reject this; the fix is `< 0`.
// ---------------------------------------------------------------------------
void test_zero_lookahead_ok() {
    auto path = write_temp(make_meta_json(R"(, "lookahead_ms": 0.0)"), "zero");
    auto m = nablafx::load_composite_meta(path);
    std::fprintf(stderr, "[zero]  lookahead_ms=%g (want 0)\n", m.ceiling.lookahead_ms);
    assert(m.ceiling.lookahead_ms == 0.0f);
    std::fprintf(stderr, "[zero]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: REGRESSION — a NEGATIVE lookahead_ms must THROW. Before the fix this
//         loaded silently and produced a negative delay_samples → OOB ring
//         buffer access in the ceiling limiter. This is the previously-broken
//         path: without the validation this assert fails (no throw).
// ---------------------------------------------------------------------------
void test_negative_lookahead_throws() {
    auto path = write_temp(make_meta_json(R"(, "lookahead_ms": -1.5)"), "neg");
    bool threw = false;
    try {
        auto m = nablafx::load_composite_meta(path);
        std::fprintf(stderr, "[neg]   NO THROW, loaded lookahead_ms=%g (BUG)\n",
                     m.ceiling.lookahead_ms);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    std::fprintf(stderr, "[neg]   threw=%d (want 1)\n", (int)threw);
    assert(threw && "negative lookahead_ms must be rejected");
    std::fprintf(stderr, "[neg]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: DEFAULT lookahead — when the field is absent the loader falls back to
//         1.5 ms, which is non-negative and must NOT throw.
// ---------------------------------------------------------------------------
void test_default_lookahead_ok() {
    auto path = write_temp(make_meta_json(""), "default");
    auto m = nablafx::load_composite_meta(path);
    std::fprintf(stderr, "[deflt] lookahead_ms=%g (want 1.5 fallback)\n",
                 m.ceiling.lookahead_ms);
    assert(m.ceiling.lookahead_ms == 1.5f);
    std::fprintf(stderr, "[deflt] PASS\n");
}

}  // namespace

int main() {
    test_valid_lookahead_loads();
    test_zero_lookahead_ok();
    test_negative_lookahead_throws();
    test_default_lookahead_ok();
    std::fprintf(stderr, "ALL 4 TESTS PASSED\n");
    return 0;
}
