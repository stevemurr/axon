// Unit tests for plugin_meta.json validation (load_meta / parse_dsp_block).
//
// These tests target the input-validation guards added to meta.cpp:
//   - sample_rate must be > 0          (division-by-zero guard)
//   - latency_samples must be >= 0     (negative latency-reporting guard)
//   - spectral_mask_eq block_size > 0  (division-by-zero guard)
//   - num_control_params == n_bands    (DSP-correctness guard)
// plus the rest of the load_meta() surface:
//   - stage_kind parsing (nn / dsp / nn+dsp / unknown / missing-defaults-to-nn)
//   - schema_version acceptance (1, 2) and rejection (0, 3, missing)
//   - schema_v1 legacy semantics (stage_kind + dsp_blocks are IGNORED)
//   - rational_a block parsing (exact round-trip, version 'A' only)
//   - unsupported dsp_block kind rejection
//   - controls round-trip incl. skew/unit defaults; state_tensors float32-only
//   - the six stage_kind <-> populated-content consistency guards
//   - missing file / malformed JSON / missing required key all throw
//
// Each "reject" test starts from a KNOWN-VALID meta JSON, mutates exactly one
// field into the previously-accepted-but-broken value, writes it to a temp file,
// and asserts that load_meta() now throws. The baseline test proves the JSON is
// otherwise well-formed, so a throw can only come from the new guard — i.e. on
// the pre-fix code these reject tests would FAIL (no throw), catching the bug.
// Guard-specific reject tests additionally pin the exception MESSAGE so the
// throw provably comes from the intended branch, not a neighbouring guard.
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
#ifdef _WIN32
#include <chrono>       // unique temp names (no mkstemp on Windows)
#include <filesystem>
#else
#include <unistd.h>   // mkstemp
#endif

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

// A complete, valid schema_version=2 pure-nn bundle: one fully-specified
// control, one control relying on skew/unit defaults, one float32 state
// tensor, an explicit trace_len, and an explicitly-empty dsp_blocks array.
const char* kValidNnMeta = R"JSON(
{
  "schema_version": 2,
  "effect_name": "nn_effect",
  "model_id": "nn_model",
  "architecture": "tcn",
  "sample_rate": 48000,
  "channels": 2,
  "causal": false,
  "receptive_field": 8192,
  "latency_samples": 4096,
  "num_controls": 2,
  "trace_len": 8192,
  "stage_kind": "nn",
  "controls": [
    {"id": "drive", "name": "Drive", "min": 0.0, "max": 10.0, "default": 2.5, "skew": 0.25, "unit": "dB"},
    {"id": "mix", "name": "Mix", "min": 0.0, "max": 1.0, "default": 0.5}
  ],
  "state_tensors": [
    {"name": "processor_lstm_h", "shape": [2, 1, 64], "dtype": "float32"}
  ],
  "input_names": ["audio_in", "controls"],
  "output_names": ["audio_out"],
  "dsp_blocks": []
}
)JSON";

// A complete, valid schema_version=2 pure-dsp bundle with one rational_a
// block. Coefficients are chosen exactly representable in binary float so the
// round-trip asserts can use exact equality.
const char* kValidDspMeta = R"JSON(
{
  "schema_version": 2,
  "effect_name": "dsp_effect",
  "model_id": "dsp_model",
  "architecture": "dsp",
  "sample_rate": 44100,
  "channels": 1,
  "causal": true,
  "receptive_field": 1,
  "latency_samples": 0,
  "num_controls": 0,
  "stage_kind": "dsp",
  "controls": [],
  "state_tensors": [],
  "input_names": [],
  "output_names": [],
  "dsp_blocks": [
    {
      "kind": "rational_a",
      "name": "processor.nonlin",
      "params": {
        "version": "A",
        "numerator": [0.5, 1.25, -2.0],
        "denominator": [0.75, 0.125]
      }
    }
  ]
}
)JSON";

// A complete, valid legacy schema_version=1 bundle: no stage_kind, no
// trace_len, no dsp_blocks — the pre-v2 single-ONNX black-box shape.
const char* kValidV1Meta = R"JSON(
{
  "schema_version": 1,
  "effect_name": "legacy_effect",
  "model_id": "legacy_model",
  "architecture": "lstm",
  "sample_rate": 44100,
  "channels": 1,
  "causal": true,
  "receptive_field": 1,
  "latency_samples": 0,
  "num_controls": 1,
  "controls": [
    {"id": "gain", "name": "Gain", "min": -12.0, "max": 12.0, "default": 0.0}
  ],
  "state_tensors": [],
  "input_names": ["audio_in"],
  "output_names": ["audio_out"]
}
)JSON";

// Write `contents` to a unique temp path and return that path. Uses mkstemp
// on POSIX so the path is created safely and uniquely; Windows has no
// mkstemp, so uses temp_directory_path + a clock nonce + counter (the test
// is single-process/single-threaded, so this cannot collide in practice).
std::string write_temp(const std::string& contents, const char* tag) {
#ifdef _WIN32
    static int counter = 0;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string path =
        (std::filesystem::temp_directory_path() /
         (std::string("test_meta_") + tag + "_" + std::to_string(nonce) +
          "_" + std::to_string(counter++)))
            .string();
#else
    std::string templ = std::string("/tmp/test_meta_") + tag + "_XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    assert(fd != -1);
    ::close(fd);
    std::string path(buf.data());
#endif
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

// Write `contents` to a temp file, load it, clean up, return the parsed meta.
// Any throw propagates (and aborts the test) — use only for must-load JSON.
nablafx::PluginMeta load_from(const std::string& contents, const char* tag) {
    std::string path = write_temp(contents, tag);
    nablafx::PluginMeta m = nablafx::load_meta(path);
    std::remove(path.c_str());
    return m;
}

// True iff loading `contents` throws std::runtime_error whose message contains
// `needle`. Pinning the message proves the throw came from the intended guard
// and not a neighbouring check on the same code path.
bool rejects_with(const std::string& contents, const char* tag, const std::string& needle) {
    std::string path = write_temp(contents, tag);
    bool ok = false;
    try {
        nablafx::load_meta(path);
    } catch (const std::runtime_error& e) {
        ok = std::string(e.what()).find(needle) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  [%s] wrong message: %s\n", tag, e.what());
    } catch (...) {
        // wrong exception type
    }
    std::remove(path.c_str());
    return ok;
}

// True iff loading `contents` throws ANY std::exception. Used where the throw
// legitimately comes from nlohmann (parse_error / out_of_range are json
// exceptions, not std::runtime_error).
bool rejects_any(const std::string& contents, const char* tag) {
    std::string path = write_temp(contents, tag);
    bool ok = false;
    try {
        nablafx::load_meta(path);
    } catch (const std::exception&) {
        ok = true;
    }
    std::remove(path.c_str());
    return ok;
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
    assert(m.effect_name == "test_effect");
    assert(m.model_id == "test_model");
    assert(m.architecture == "lstm");
    assert(m.sample_rate == 44100);
    assert(m.channels == 1);
    assert(m.causal == true);
    assert(m.receptive_field == 1);
    assert(m.latency_samples == 0);
    assert(m.num_controls == 0);
    assert(m.trace_len == 0);
    assert(m.stage_kind == nablafx::StageKind::NnDsp);
    assert(m.controls.empty());
    assert(m.state_tensors.empty());
    assert(m.input_names == std::vector<std::string>{"audio_in"});
    assert(m.output_names == std::vector<std::string>{"params_proc_0"});
    assert(m.dsp_blocks.size() == 1);
    assert(m.dsp_blocks[0].kind == "spectral_mask_eq");
    assert(m.dsp_blocks[0].name == "processor.processors.0");
    const auto& sm = std::get<nablafx::SpectralMaskEqParams>(m.dsp_blocks[0].params);
    assert(sm.sample_rate == 44100);
    assert(sm.block_size == 128);
    assert(sm.num_control_params == 64);
    assert(sm.n_fft == 4096);
    assert(sm.hop == 2048);
    assert(sm.n_bands == 64);
    assert(sm.num_control_params == sm.n_bands);
    // These literals are exactly representable in binary float — exact holds.
    assert(sm.min_gain_db == -18.0f);
    assert(sm.max_gain_db == 18.0f);
    assert(sm.f_min == 30.0f);
    assert(sm.f_max == 22050.0f);
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

// ---------------------------------------------------------------------------
// Test 5: stage_kind parsing — "nn" and "dsp" parse to their enum values,
//         an unknown token is rejected, and a MISSING stage_kind in a v2
//         bundle defaults to "nn" (the documented j.value() fallback).
//         ("nn+dsp" is proven by the baseline test.)
// ---------------------------------------------------------------------------
void test_stage_kind_variants() {
    // "nn" parses to StageKind::Nn.
    nablafx::PluginMeta nn = load_from(kValidNnMeta, "sk_nn");
    assert(nn.stage_kind == nablafx::StageKind::Nn);

    // "dsp" parses to StageKind::Dsp.
    nablafx::PluginMeta dsp = load_from(kValidDspMeta, "sk_dsp");
    assert(dsp.stage_kind == nablafx::StageKind::Dsp);

    // Unknown token is rejected by parse_stage_kind.
    std::string bad = replace_once(kValidNnMeta,
                                   "\"stage_kind\": \"nn\",",
                                   "\"stage_kind\": \"cnn\",");
    assert(rejects_with(bad, "sk_unknown", "unknown stage_kind"));

    // v2 bundle with NO stage_kind key defaults to "nn" and still loads
    // (the kValidNnMeta content is nn-shaped, so the sanity check passes).
    std::string nokey = replace_once(kValidNnMeta,
                                     "  \"stage_kind\": \"nn\",\n", "");
    nablafx::PluginMeta def = load_from(nokey, "sk_default");
    assert(def.stage_kind == nablafx::StageKind::Nn);
    std::fprintf(stderr, "[stage_kind] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 6: schema_version 1 legacy semantics — v1 loads as a pure-nn bundle
//         with trace_len defaulting to 0, and both "stage_kind" and
//         "dsp_blocks" keys are IGNORED if present (v1 predates them).
// ---------------------------------------------------------------------------
void test_v1_legacy_semantics() {
    nablafx::PluginMeta m = load_from(kValidV1Meta, "v1_valid");
    assert(m.schema_version == 1);
    assert(m.stage_kind == nablafx::StageKind::Nn);
    assert(m.trace_len == 0);          // key absent -> legacy default
    assert(m.dsp_blocks.empty());
    assert(m.controls.size() == 1);
    assert(m.controls[0].id == "gain");

    // v1 + "stage_kind": "dsp" must be IGNORED (forced Nn). If load_meta ever
    // started honouring it for v1, the dsp-with-empty-dsp_blocks sanity check
    // would throw, so a successful Nn load pins the version gate.
    std::string with_sk = replace_once(kValidV1Meta,
                                       "\"schema_version\": 1,",
                                       "\"schema_version\": 1,\n  \"stage_kind\": \"dsp\",");
    nablafx::PluginMeta m2 = load_from(with_sk, "v1_stagekind");
    assert(m2.stage_kind == nablafx::StageKind::Nn);

    // v1 + "dsp_blocks" must be IGNORED, not parsed. The injected block has an
    // unsupported version 'Z', so if v1 blocks were ever parsed this would
    // throw; and if they were parsed AND accepted, the nn-with-nonempty-blocks
    // sanity check would throw. A clean load with empty dsp_blocks pins both.
    std::string with_blocks = replace_once(kValidV1Meta,
        "\"state_tensors\": [],",
        "\"state_tensors\": [],\n"
        "  \"dsp_blocks\": [{\"kind\": \"rational_a\", \"name\": \"x\","
        " \"params\": {\"version\": \"Z\", \"numerator\": [1.0], \"denominator\": []}}],");
    nablafx::PluginMeta m3 = load_from(with_blocks, "v1_dspblocks");
    assert(m3.dsp_blocks.empty());
    std::fprintf(stderr, "[v1]     PASS\n");
}

// ---------------------------------------------------------------------------
// Test 7: schema_version gate — 0, 3 and a MISSING key (which defaults to 0)
//         are all rejected with the version-specific message.
// ---------------------------------------------------------------------------
void test_reject_bad_schema_version() {
    std::string v0 = replace_once(kValidNnMeta,
                                  "\"schema_version\": 2,",
                                  "\"schema_version\": 0,");
    assert(rejects_with(v0, "sv_zero", "unsupported plugin_meta schema_version"));

    std::string v3 = replace_once(kValidNnMeta,
                                  "\"schema_version\": 2,",
                                  "\"schema_version\": 3,");
    assert(rejects_with(v3, "sv_three", "unsupported plugin_meta schema_version"));

    std::string vnone = replace_once(kValidNnMeta,
                                     "  \"schema_version\": 2,\n", "");
    assert(rejects_with(vnone, "sv_missing", "unsupported plugin_meta schema_version"));
    std::fprintf(stderr, "[schema] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 8: file/JSON-level failures — nonexistent path throws the "failed to
//         open" runtime_error; truncated JSON and a missing required key both
//         throw (nlohmann exceptions derive from std::exception, and load_meta
//         documents "throws on any problem").
// ---------------------------------------------------------------------------
void test_missing_or_malformed_file() {
    // Nonexistent path — must be load_meta's own guard, with its message.
    bool threw = false;
    try {
        nablafx::load_meta("/tmp/test_meta_definitely_nonexistent_zq7/x.json");
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("failed to open") != std::string::npos;
    }
    assert(threw);

    // Truncated JSON -> parse error surfaces as std::exception.
    assert(rejects_any("{ \"schema_version\": 2, ", "truncated"));

    // Structurally valid JSON missing a REQUIRED key (effect_name) throws.
    std::string noname = replace_once(kValidNnMeta,
                                      "  \"effect_name\": \"nn_effect\",\n", "");
    assert(rejects_any(noname, "missing_key"));
    std::fprintf(stderr, "[file]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 9: rational_a block parsing — exact coefficient round-trip (all
//         literals exactly representable in binary float), version key
//         optional (defaults 'A'), version != 'A' rejected, and an unknown
//         dsp_block kind rejected.
// ---------------------------------------------------------------------------
void test_rational_a_parsing() {
    nablafx::PluginMeta m = load_from(kValidDspMeta, "ra_valid");
    assert(m.dsp_blocks.size() == 1);
    assert(m.dsp_blocks[0].kind == "rational_a");
    assert(m.dsp_blocks[0].name == "processor.nonlin");
    const auto& r = std::get<nablafx::RationalAParams>(m.dsp_blocks[0].params);
    assert((r.numerator == std::vector<float>{0.5f, 1.25f, -2.0f}));
    assert((r.denominator == std::vector<float>{0.75f, 0.125f}));

    // "version" omitted defaults to "A" and is accepted.
    std::string nover = replace_once(kValidDspMeta,
                                     "        \"version\": \"A\",\n", "");
    nablafx::PluginMeta m2 = load_from(nover, "ra_nover");
    assert(std::get<nablafx::RationalAParams>(m2.dsp_blocks[0].params).numerator.size() == 3);

    // version "B" is rejected with the version-specific message.
    std::string vb = replace_once(kValidDspMeta,
                                  "\"version\": \"A\",",
                                  "\"version\": \"B\",");
    assert(rejects_with(vb, "ra_verB", "only Rational version 'A' is supported"));

    // Unknown block kind is rejected.
    std::string badkind = replace_once(kValidDspMeta,
                                       "\"kind\": \"rational_a\",",
                                       "\"kind\": \"waveshaper\",");
    assert(rejects_with(badkind, "ra_badkind", "unsupported dsp_block kind"));
    std::fprintf(stderr, "[rational_a] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 10: controls + state_tensors round-trip. First control specifies every
//          field; second omits skew/unit and must get the documented defaults
//          (skew=1.0, unit=""). State tensor round-trips name/shape/dtype and
//          any non-float32 dtype is rejected. All float literals are exactly
//          representable, so exact equality holds.
// ---------------------------------------------------------------------------
void test_controls_and_state_tensors() {
    nablafx::PluginMeta m = load_from(kValidNnMeta, "ctl_valid");

    assert(m.controls.size() == 2);
    const auto& c0 = m.controls[0];
    assert(c0.id == "drive");
    assert(c0.name == "Drive");
    assert(c0.min == 0.0f);
    assert(c0.max == 10.0f);
    assert(c0.def == 2.5f);
    assert(c0.skew == 0.25f);
    assert(c0.unit == "dB");
    const auto& c1 = m.controls[1];
    assert(c1.id == "mix");
    assert(c1.def == 0.5f);
    assert(c1.skew == 1.0f);   // default when key absent
    assert(c1.unit.empty());   // default when key absent

    assert(m.state_tensors.size() == 1);
    const auto& st = m.state_tensors[0];
    assert(st.name == "processor_lstm_h");
    assert((st.shape == std::vector<int64_t>{2, 1, 64}));
    assert(st.dtype == "float32");

    // Remaining top-level fields of the nn template round-trip too.
    assert(m.effect_name == "nn_effect");
    assert(m.architecture == "tcn");
    assert(m.sample_rate == 48000);
    assert(m.channels == 2);
    assert(m.causal == false);
    assert(m.receptive_field == 8192);
    assert(m.latency_samples == 4096);
    assert(m.num_controls == 2);
    assert(m.trace_len == 8192);   // explicit value, not the 0 default
    assert((m.input_names == std::vector<std::string>{"audio_in", "controls"}));
    assert((m.output_names == std::vector<std::string>{"audio_out"}));

    // Non-float32 state tensors are rejected with the dtype guard message.
    std::string f64 = replace_once(kValidNnMeta,
                                   "\"dtype\": \"float32\"",
                                   "\"dtype\": \"float64\"");
    assert(rejects_with(f64, "st_f64", "only float32 is supported"));
    std::fprintf(stderr, "[controls/state] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 11: the six stage_kind <-> content consistency guards. Each mutation
//          violates exactly ONE guard, and the pinned message proves the
//          intended branch fired.
// ---------------------------------------------------------------------------
void test_stage_content_consistency() {
    const char* kBlock =
        "\"dsp_blocks\": [{\"kind\": \"rational_a\", \"name\": \"x\","
        " \"params\": {\"numerator\": [1.0], \"denominator\": []}}]";

    // nn + non-empty dsp_blocks.
    std::string nn_blocks = replace_once(kValidNnMeta, "\"dsp_blocks\": []", kBlock);
    assert(rejects_with(nn_blocks, "nn_blocks",
                        "stage_kind=nn but dsp_blocks is non-empty"));

    // nn + empty input_names.
    std::string nn_noin = replace_once(kValidNnMeta,
                                       "\"input_names\": [\"audio_in\", \"controls\"],",
                                       "\"input_names\": [],");
    assert(rejects_with(nn_noin, "nn_noin",
                        "stage_kind=nn but input_names is empty"));

    // dsp + empty dsp_blocks (retarget the nn template: its input_names must
    // also be emptied so ONLY the dsp_blocks guard can fire).
    std::string dsp_noblocks = replace_once(kValidNnMeta,
                                            "\"stage_kind\": \"nn\",",
                                            "\"stage_kind\": \"dsp\",");
    dsp_noblocks = replace_once(dsp_noblocks,
                                "\"input_names\": [\"audio_in\", \"controls\"],",
                                "\"input_names\": [],");
    assert(rejects_with(dsp_noblocks, "dsp_noblocks",
                        "stage_kind=dsp but dsp_blocks is empty"));

    // dsp + non-empty input_names.
    std::string dsp_in = replace_once(kValidDspMeta,
                                      "\"input_names\": [],",
                                      "\"input_names\": [\"audio_in\"],");
    assert(rejects_with(dsp_in, "dsp_in",
                        "stage_kind=dsp but input_names is non-empty"));

    // nn+dsp + empty dsp_blocks (nn template keeps its input_names, so only
    // the dsp_blocks guard can fire).
    std::string nd_noblocks = replace_once(kValidNnMeta,
                                           "\"stage_kind\": \"nn\",",
                                           "\"stage_kind\": \"nn+dsp\",");
    assert(rejects_with(nd_noblocks, "nd_noblocks",
                        "stage_kind=nn+dsp but dsp_blocks is empty"));

    // nn+dsp + empty input_names (baseline template keeps its dsp block).
    std::string nd_noin = replace_once(kValidMeta,
                                       "\"input_names\": [\"audio_in\"],",
                                       "\"input_names\": [],");
    assert(rejects_with(nd_noin, "nd_noin",
                        "stage_kind=nn+dsp but input_names is empty"));
    std::fprintf(stderr, "[consistency] PASS\n");
}

}  // namespace

int main() {
    test_valid_meta_loads();
    test_reject_zero_sample_rate();
    test_reject_negative_latency();
    test_reject_zero_block_size();
    test_reject_control_params_band_mismatch();
    test_stage_kind_variants();
    test_v1_legacy_semantics();
    test_reject_bad_schema_version();
    test_missing_or_malformed_file();
    test_rational_a_parsing();
    test_controls_and_state_tensors();
    test_stage_content_consistency();
    std::fprintf(stderr, "ALL 12 TESTS PASSED\n");
    return 0;
}
