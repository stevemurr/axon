// Unit tests for plugin_meta.json validation (load_meta / parse_dsp_block).
//
// These tests target the input-validation guards added to meta.cpp:
//   - sample_rate must be > 0          (division-by-zero guard)
//   - latency_samples must be >= 0     (negative latency-reporting guard)
//   - spectral_mask_eq block_size > 0  (division-by-zero guard)
//   - num_control_params == n_bands    (DSP-correctness guard)
//
// Each "reject" test starts from a KNOWN-VALID meta JSON, mutates exactly one
// field into the previously-accepted-but-broken value, writes it to a temp file,
// and asserts that load_meta() now throws. The baseline test proves the JSON is
// otherwise well-formed, so a throw can only come from the new guard — i.e. on
// the pre-fix code these reject tests would FAIL (no throw), catching the bug.
//
//   g++ -O2 -std=c++17 -I src -I <json/single_include> \
//       tests/test_meta.cpp src/meta.cpp -o tests/test_meta && tests/test_meta

#include "../src/meta.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>   // mkstemp

namespace {

// A complete, valid schema_version=2 nn+dsp bundle with one spectral_mask_eq
// block. Every reject test below is a single-field mutation of this string.
const char* kValidMeta = R"JSON(
{
  "schema_version": 2,
  "effect_name": "test_effect",
  "model_id": "test_model",
  "architecture": "lstm",
  "sample_rate": 44100,
  "channels": 1,
  "causal": true,
  "receptive_field": 1,
  "latency_samples": 0,
  "num_controls": 0,
  "trace_len": 0,
  "stage_kind": "nn+dsp",
  "controls": [],
  "state_tensors": [],
  "input_names": ["audio_in"],
  "output_names": ["params_proc_0"],
  "dsp_blocks": [
    {
      "kind": "spectral_mask_eq",
      "name": "processor.processors.0",
      "params": {
        "sample_rate": 44100,
        "block_size": 128,
        "num_control_params": 64,
        "n_fft": 4096,
        "hop": 2048,
        "n_bands": 64,
        "min_gain_db": -18.0,
        "max_gain_db": 18.0,
        "f_min": 30.0,
        "f_max": 22050.0
      }
    }
  ]
}
)JSON";

// Write `contents` to a unique temp path and return that path. Uses mkstemp so
// the path is created safely and uniquely.
std::string write_temp(const std::string& contents, const char* tag) {
    std::string templ = std::string("/tmp/test_meta_") + tag + "_XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    assert(fd != -1);
    ::close(fd);
    std::string path(buf.data());
    std::ofstream f(path);
    assert(f.is_open());
    f << contents;
    f.close();
    return path;
}

// Replace the first occurrence of `from` with `to` in `s`. Asserts it existed,
// so a test can never silently pass by mutating nothing.
std::string replace_once(std::string s, const std::string& from, const std::string& to) {
    auto pos = s.find(from);
    assert(pos != std::string::npos && "mutation token not found in valid JSON");
    s.replace(pos, from.size(), to);
    return s;
}

// Returns true iff load_meta(path) threw std::runtime_error.
bool throws_runtime_error(const std::string& path) {
    try {
        nablafx::load_meta(path);
    } catch (const std::runtime_error&) {
        return true;
    } catch (...) {
        return false;  // wrong exception type — not the guard we want
    }
    return false;
}

// ---------------------------------------------------------------------------
// Test 0: BASELINE — the unmutated valid JSON loads cleanly and round-trips the
//         fields the guards inspect. Proves later throws are from the guards,
//         not from malformed JSON.
// ---------------------------------------------------------------------------
void test_valid_meta_loads() {
    std::string path = write_temp(kValidMeta, "valid");
    nablafx::PluginMeta m = nablafx::load_meta(path);
    std::remove(path.c_str());

    assert(m.schema_version == 2);
    assert(m.sample_rate == 44100);
    assert(m.latency_samples == 0);
    assert(m.dsp_blocks.size() == 1);
    const auto& sm = std::get<nablafx::SpectralMaskEqParams>(m.dsp_blocks[0].params);
    assert(sm.block_size == 128);
    assert(sm.num_control_params == sm.n_bands);
    std::fprintf(stderr, "[valid] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 1: sample_rate <= 0 is REJECTED (division-by-zero guard). Pre-fix this
//         was accepted and would later divide by zero downstream.
// ---------------------------------------------------------------------------
void test_reject_zero_sample_rate() {
    // top-level "sample_rate": 44100  ->  0 . Use the leading newline+indent to
    // avoid matching the nested block-params sample_rate.
    std::string j = replace_once(kValidMeta,
                                 "\"sample_rate\": 44100,\n  \"channels\"",
                                 "\"sample_rate\": 0,\n  \"channels\"");
    std::string path = write_temp(j, "sr_zero");
    bool threw = throws_runtime_error(path);
    std::remove(path.c_str());
    std::fprintf(stderr, "[sr=0]   threw=%d (want 1)\n", threw);
    assert(threw);

    // Also the negative case.
    std::string j2 = replace_once(kValidMeta,
                                  "\"sample_rate\": 44100,\n  \"channels\"",
                                  "\"sample_rate\": -1,\n  \"channels\"");
    std::string p2 = write_temp(j2, "sr_neg");
    bool threw2 = throws_runtime_error(p2);
    std::remove(p2.c_str());
    std::fprintf(stderr, "[sr<0]   threw=%d (want 1)\n", threw2);
    assert(threw2);
    std::fprintf(stderr, "[sr]     PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: latency_samples < 0 is REJECTED. Pre-fix a negative latency would be
//         reported to the host (incorrect plugin latency).
// ---------------------------------------------------------------------------
void test_reject_negative_latency() {
    std::string j = replace_once(kValidMeta,
                                 "\"latency_samples\": 0,",
                                 "\"latency_samples\": -10,");
    std::string path = write_temp(j, "lat_neg");
    bool threw = throws_runtime_error(path);
    std::remove(path.c_str());
    std::fprintf(stderr, "[lat<0]  threw=%d (want 1)\n", threw);
    assert(threw);

    // Sanity: latency_samples == 0 is still allowed (the >= 0 boundary).
    std::string path0 = write_temp(kValidMeta, "lat_zero");
    bool ok = !throws_runtime_error(path0);
    std::remove(path0.c_str());
    assert(ok);
    std::fprintf(stderr, "[lat]    PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: spectral_mask_eq block_size <= 0 is REJECTED (division-by-zero guard
//         in parse_dsp_block). This is the nested-params block_size.
// ---------------------------------------------------------------------------
void test_reject_zero_block_size() {
    std::string j = replace_once(kValidMeta,
                                 "\"block_size\": 128,",
                                 "\"block_size\": 0,");
    std::string path = write_temp(j, "bs_zero");
    bool threw = throws_runtime_error(path);
    std::remove(path.c_str());
    std::fprintf(stderr, "[bs=0]   threw=%d (want 1)\n", threw);
    assert(threw);

    std::string j2 = replace_once(kValidMeta,
                                  "\"block_size\": 128,",
                                  "\"block_size\": -64,");
    std::string p2 = write_temp(j2, "bs_neg");
    bool threw2 = throws_runtime_error(p2);
    std::remove(p2.c_str());
    std::fprintf(stderr, "[bs<0]   threw=%d (want 1)\n", threw2);
    assert(threw2);
    std::fprintf(stderr, "[bs]     PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: num_control_params != n_bands is REJECTED (DSP-correctness guard).
//         The controller emits n_bands values; a mismatch silently misaligns
//         the mask vs the filterbank. Mutate num_control_params away from 64.
// ---------------------------------------------------------------------------
void test_reject_control_params_band_mismatch() {
    std::string j = replace_once(kValidMeta,
                                 "\"num_control_params\": 64,",
                                 "\"num_control_params\": 32,");
    std::string path = write_temp(j, "ncp_mismatch");
    bool threw = throws_runtime_error(path);
    std::remove(path.c_str());
    std::fprintf(stderr, "[ncp!=nb] threw=%d (want 1)\n", threw);
    assert(threw);

    // Mismatch the other direction (mutate n_bands instead) to prove the check
    // is symmetric and not pinned to one field's value.
    std::string j2 = replace_once(kValidMeta,
                                  "\"n_bands\": 64,",
                                  "\"n_bands\": 128,");
    std::string p2 = write_temp(j2, "nb_mismatch");
    bool threw2 = throws_runtime_error(p2);
    std::remove(p2.c_str());
    std::fprintf(stderr, "[nb!=ncp] threw=%d (want 1)\n", threw2);
    assert(threw2);
    std::fprintf(stderr, "[ncp]    PASS\n");
}

}  // namespace

int main() {
    test_valid_meta_loads();
    test_reject_zero_sample_rate();
    test_reject_negative_latency();
    test_reject_zero_block_size();
    test_reject_control_params_band_mismatch();
    std::fprintf(stderr, "ALL 5 TESTS PASSED\n");
    return 0;
}
