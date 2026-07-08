// Self-contained validation tests for the composite axon_meta.json loader.
// Unlike test_composite_meta.cpp (which needs a real staged fixture), this
// writes its own temp JSON files so it can exercise malformed/edge inputs.
//
// Covered behavior of nablafx::load_composite_meta():
//   - lookahead_ms guard (regression: negative values used to be accepted,
//     producing a negative delay_samples and OOB ring-buffer access in the
//     ceiling limiter; the loader must THROW on lookahead_ms < 0)
//   - missing file / malformed JSON rejection
//   - schema_version gate (only 2 is accepted; absent defaults to 0)
//   - auto_eq cross-validation (empty class_order, default_class and
//     class_order entries must exist in the classes map)
//   - controls parsing: explicit fields round-trip exactly; id falls back to
//     the JSON object key, skew to 1.0, unit to ""
//   - optional amount_mapping.ssl_comp section (absent / empty / explicit)
//   - ceiling attack/release/lookahead defaults when fields are absent
//   - full-fixture field round-trip, class_order array-order preservation,
//     and load determinism (two loads of the same file agree exactly)
//
//   g++ -O2 -std=c++17 -I../src -I<json>/single_include -UNDEBUG \
//       test_composite_meta_validate.cpp ../src/composite_meta.cpp \
//       ../src/meta.cpp -o test_composite_meta_validate

#include "../src/composite_meta.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#ifdef _WIN32
#include <filesystem>
#endif
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
#ifdef _WIN32
    // No /tmp on Windows — same std::filesystem pattern as test_meta.cpp /
    // test_ort_session.cpp. POSIX branch below kept verbatim.
    const std::string path =
        (std::filesystem::temp_directory_path() /
         (std::string("axon_meta_test_") + tag + ".json"))
            .string();
#else
    std::string path = std::string("/tmp/axon_meta_test_") + tag + ".json";
#endif
    std::ofstream o(path, std::ios::trunc);
    o << contents;
    o.close();
    return path;
}

// Section-replaceable fixture builder for the validation tests below. Each
// part is a complete `"key": value` JSON fragment so a test can swap exactly
// the section under test and keep everything else loadable.
struct MetaParts {
    std::string schema = R"("schema_version": 2)";
    std::string sub_bundles =
        R"("sub_bundles": { "saturator": "sat_dir" })";
    std::string auto_eq =
        R"("auto_eq": {
             "default_class": "vocal",
             "class_order": ["vocal"],
             "classes": { "vocal": "aeq_vocal_dir" }
           })";
    std::string controls = R"("controls": {})";
    std::string amount_mapping =
        R"("amount_mapping": {
             "saturator": { "pre_gain_db_max": 12.0,
                            "post_gain_db_max": -12.0,
                            "wet_mix_max": 1.0 },
             "auto_eq":   { "wet_mix_max": 1.0 }
           })";
    std::string ceiling =
        R"("ceiling": { "ceiling_dbtp": -1.0, "lookahead_ms": 1.5,
                        "attack_ms": 0.5, "release_ms": 50.0 })";
};

std::string make_meta_json_parts(const MetaParts& p) {
    return "{\n" + p.schema + R"(,
  "effect_name": "Axon",
  "model_id": "test-model",
  "sample_rate": 44100,
  "channels": 2,
  )" + p.sub_bundles + ",\n  " + p.auto_eq + ",\n  " + p.controls + ",\n  "
       + p.amount_mapping + R"(,
  "leveler": { "target_lufs": -14.0 },
  )" + p.ceiling + "\n}";
}

// Load `path` and require a std::runtime_error whose message contains
// `needle` — a throw for the wrong reason (e.g. a missing key surfacing as a
// json .at() error) must NOT count as a pass.
void expect_runtime_error_containing(const std::string& path,
                                     const char* needle, const char* tag) {
    bool threw_right = false;
    try {
        (void)nablafx::load_composite_meta(path);
        std::fprintf(stderr, "[%s] NO THROW (BUG)\n", tag);
    } catch (const std::runtime_error& e) {
        threw_right = std::strstr(e.what(), needle) != nullptr;
        std::fprintf(stderr, "[%s] what()=\"%s\" contains \"%s\": %d\n",
                     tag, e.what(), needle, (int)threw_right);
    }
    assert(threw_right && "must throw std::runtime_error mentioning the cause");
}

const nablafx::ControlSpec& find_control(
        const std::vector<nablafx::ControlSpec>& v, const std::string& id) {
    auto it = std::find_if(v.begin(), v.end(),
                           [&](const nablafx::ControlSpec& c) { return c.id == id; });
    assert(it != v.end() && "control id must be present");
    return *it;
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

// ---------------------------------------------------------------------------
// Test 5: MISSING FILE — a nonexistent path must throw std::runtime_error
//         with the "failed to open" message (not crash, not return defaults).
// ---------------------------------------------------------------------------
void test_missing_file_throws() {
    expect_runtime_error_containing(
        "/tmp/axon_meta_test_no_such_dir/definitely_missing.json",
        "failed to open", "nofile");
    std::fprintf(stderr, "[nofile] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 6: MALFORMED JSON — syntactically invalid content must surface as an
//         exception from the parse (nlohmann's parse_error derives from
//         std::exception, not std::runtime_error), never as a loaded meta.
// ---------------------------------------------------------------------------
void test_malformed_json_throws() {
    auto path = write_temp("{ this is : not json !!", "malformed");
    bool threw = false;
    try {
        (void)nablafx::load_composite_meta(path);
    } catch (const std::exception& e) {
        threw = true;
        std::fprintf(stderr, "[badjson] what()=\"%s\"\n", e.what());
    }
    assert(threw && "malformed JSON must throw");
    std::fprintf(stderr, "[badjson] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 7: SCHEMA GATE — schema_version 1 and 3 must be rejected with a
//         message naming the offending version; an ABSENT schema_version
//         defaults to 0 and must be rejected the same way (a pre-schema file
//         must not be misread as v2).
// ---------------------------------------------------------------------------
void test_unsupported_schema_version_throws() {
    MetaParts p;
    p.schema = R"("schema_version": 1)";
    expect_runtime_error_containing(
        write_temp(make_meta_json_parts(p), "schema_v1"),
        "schema_version 1", "schema1");

    p.schema = R"("schema_version": 3)";
    expect_runtime_error_containing(
        write_temp(make_meta_json_parts(p), "schema_v3"),
        "schema_version 3", "schema3");

    // Absent field → j.value("schema_version", 0) → 0 → rejected as version 0.
    p.schema = R"("_schema_version_absent": true)";
    expect_runtime_error_containing(
        write_temp(make_meta_json_parts(p), "schema_absent"),
        "schema_version 0", "schema0");
    std::fprintf(stderr, "[schema] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 8: AUTO-EQ — empty class_order must be rejected. An empty order list
//         would leave the integer-valued CLS control with no valid index.
// ---------------------------------------------------------------------------
void test_empty_class_order_throws() {
    MetaParts p;
    p.auto_eq = R"("auto_eq": {
        "default_class": "vocal",
        "class_order": [],
        "classes": { "vocal": "aeq_vocal_dir" }
    })";
    expect_runtime_error_containing(
        write_temp(make_meta_json_parts(p), "empty_order"),
        "class_order is empty", "aeq_empty");
    std::fprintf(stderr, "[aeq_empty] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 9: AUTO-EQ — default_class must exist in the classes map, otherwise
//         the plugin would boot pointing at a bundle dir that does not exist.
// ---------------------------------------------------------------------------
void test_default_class_not_in_classes_throws() {
    MetaParts p;
    p.auto_eq = R"("auto_eq": {
        "default_class": "drums",
        "class_order": ["vocal"],
        "classes": { "vocal": "aeq_vocal_dir" }
    })";
    expect_runtime_error_containing(
        write_temp(make_meta_json_parts(p), "bad_default_class"),
        "default_class 'drums'", "aeq_defcls");
    std::fprintf(stderr, "[aeq_defcls] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 10: AUTO-EQ — every class_order entry must exist in the classes map.
//          default_class is valid here so the failure is attributable to the
//          class_order cross-check, not the default_class check before it.
// ---------------------------------------------------------------------------
void test_class_order_entry_not_in_classes_throws() {
    MetaParts p;
    p.auto_eq = R"("auto_eq": {
        "default_class": "vocal",
        "class_order": ["vocal", "drums"],
        "classes": { "vocal": "aeq_vocal_dir" }
    })";
    expect_runtime_error_containing(
        write_temp(make_meta_json_parts(p), "bad_order_entry"),
        "class_order entry 'drums'", "aeq_order");
    std::fprintf(stderr, "[aeq_order] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 11: CONTROLS — explicit fields round-trip exactly (all fixture values
//          are binary-exact floats); omitted optional fields take their
//          documented defaults: id ← JSON object key, skew ← 1.0, unit ← "".
// ---------------------------------------------------------------------------
void test_controls_full_and_defaulted() {
    MetaParts p;
    p.controls = R"("controls": {
        "amount": { "id": "amt", "name": "Amount", "min": -24.0, "max": 24.0,
                    "default": 3.5, "skew": 0.25, "unit": "dB" },
        "trim":   { "name": "Trim", "min": 0.0, "max": 1.0, "default": 0.5 }
    })";
    auto m = nablafx::load_composite_meta(
        write_temp(make_meta_json_parts(p), "controls"));
    assert(m.controls.size() == 2);

    const auto& a = find_control(m.controls, "amt");   // explicit id wins
    assert(a.name == "Amount");
    assert(a.min == -24.0f && a.max == 24.0f && a.def == 3.5f);
    assert(a.skew == 0.25f);
    assert(a.unit == "dB");

    const auto& t = find_control(m.controls, "trim");  // id ← object key
    assert(t.name == "Trim");
    assert(t.min == 0.0f && t.max == 1.0f && t.def == 0.5f);
    assert(t.skew == 1.0f);       // default skew
    assert(t.unit.empty());       // default unit
    std::fprintf(stderr, "[controls] PASS (2 controls, defaults applied)\n");
}

// ---------------------------------------------------------------------------
// Test 12: SSL_COMP amount-mapping section is OPTIONAL (older bundles omit
//          it). Absent → struct default 1.0; present-but-empty → value()
//          default 1.0; explicit value → read exactly.
// ---------------------------------------------------------------------------
void test_ssl_comp_section_optional() {
    MetaParts p;  // default amount_mapping has NO ssl_comp section
    auto m_absent = nablafx::load_composite_meta(
        write_temp(make_meta_json_parts(p), "sslc_absent"));
    assert(m_absent.amt_ssl_comp.wet_mix_max == 1.0f);

    p.amount_mapping =
        R"("amount_mapping": {
             "saturator": { "pre_gain_db_max": 12.0,
                            "post_gain_db_max": -12.0,
                            "wet_mix_max": 1.0 },
             "auto_eq":   { "wet_mix_max": 1.0 },
             "ssl_comp":  {}
           })";
    auto m_empty = nablafx::load_composite_meta(
        write_temp(make_meta_json_parts(p), "sslc_empty"));
    assert(m_empty.amt_ssl_comp.wet_mix_max == 1.0f);

    p.amount_mapping =
        R"("amount_mapping": {
             "saturator": { "pre_gain_db_max": 6.0,
                            "post_gain_db_max": -6.0,
                            "wet_mix_max": 0.5 },
             "auto_eq":   { "wet_mix_max": 0.75 },
             "ssl_comp":  { "wet_mix_max": 0.25 }
           })";
    auto m_set = nablafx::load_composite_meta(
        write_temp(make_meta_json_parts(p), "sslc_set"));
    assert(m_set.amt_ssl_comp.wet_mix_max == 0.25f);
    // The sibling section must be read from the same object, not defaults.
    // (A legacy amount_mapping.saturator, if present, is ignored — the stage
    // was removed 2026-07.)
    assert(m_set.amt_autoeq.wet_mix_max == 0.75f);
    std::fprintf(stderr, "[ssl_comp] PASS (absent/empty/explicit)\n");
}

// ---------------------------------------------------------------------------
// Test 13: CEILING defaults — with only the required ceiling_dbtp present,
//          lookahead/attack/release must take the documented defaults
//          (1.5 / 0.5 / 50 ms), and the lookahead guard must not fire.
// ---------------------------------------------------------------------------
void test_ceiling_defaults() {
    MetaParts p;
    p.ceiling = R"("ceiling": { "ceiling_dbtp": -0.5 })";
    auto m = nablafx::load_composite_meta(
        write_temp(make_meta_json_parts(p), "ceiling_min"));
    assert(m.ceiling.ceiling_dbtp == -0.5f);
    assert(m.ceiling.lookahead_ms == 1.5f);
    assert(m.ceiling.attack_ms == 0.5f);
    assert(m.ceiling.release_ms == 50.0f);
    std::fprintf(stderr, "[ceiling] PASS (defaults 1.5/0.5/50)\n");
}

// ---------------------------------------------------------------------------
// Test 14: FULL ROUND-TRIP + DETERMINISM — a multi-class fixture loads with
//          every field exact; class_order preserves ARRAY order (not
//          alphabetical); loading the same file twice yields identical meta.
// ---------------------------------------------------------------------------
void test_full_fixture_roundtrip_and_determinism() {
    MetaParts p;
    p.sub_bundles =
        R"("sub_bundles": { "saturator": "sat_dir", "ssl_comp": "sslc_dir" })";
    p.auto_eq = R"("auto_eq": {
        "default_class": "master",
        "class_order": ["vocal", "master", "drums"],
        "classes": { "vocal": "aeq_v", "master": "aeq_m", "drums": "aeq_d" }
    })";
    auto path = write_temp(make_meta_json_parts(p), "full");

    auto m = nablafx::load_composite_meta(path);
    assert(m.schema_version == 2);
    assert(m.effect_name == "Axon");
    assert(m.model_id == "test-model");
    assert(m.sample_rate == 44100);
    assert(m.channels == 2);
    assert(m.sub_bundles.size() == 2);
    assert(m.sub_bundles.at("saturator") == "sat_dir");
    assert(m.sub_bundles.at("ssl_comp") == "sslc_dir");
    assert(m.auto_eq.default_class == "master");
    // Array order, exactly as authored — CLS control indexes via this order.
    assert(m.auto_eq.class_order.size() == 3);
    assert(m.auto_eq.class_order[0] == "vocal");
    assert(m.auto_eq.class_order[1] == "master");
    assert(m.auto_eq.class_order[2] == "drums");
    assert(m.auto_eq.classes.size() == 3);
    assert(m.auto_eq.classes.at("vocal") == "aeq_v");
    assert(m.auto_eq.classes.at("master") == "aeq_m");
    assert(m.auto_eq.classes.at("drums") == "aeq_d");
    assert(m.leveler.target_lufs == -14.0f);

    // Determinism: a second load of the same file must agree field-for-field.
    auto m2 = nablafx::load_composite_meta(path);
    assert(m2.schema_version == m.schema_version);
    assert(m2.effect_name == m.effect_name);
    assert(m2.model_id == m.model_id);
    assert(m2.sample_rate == m.sample_rate);
    assert(m2.channels == m.channels);
    assert(m2.sub_bundles == m.sub_bundles);
    assert(m2.auto_eq.default_class == m.auto_eq.default_class);
    assert(m2.auto_eq.class_order == m.auto_eq.class_order);
    assert(m2.auto_eq.classes == m.auto_eq.classes);
    assert(m2.controls.size() == m.controls.size());
    assert(m2.amt_autoeq.wet_mix_max == m.amt_autoeq.wet_mix_max);
    assert(m2.amt_ssl_comp.wet_mix_max == m.amt_ssl_comp.wet_mix_max);
    assert(m2.leveler.target_lufs == m.leveler.target_lufs);
    assert(m2.ceiling.ceiling_dbtp == m.ceiling.ceiling_dbtp);
    assert(m2.ceiling.lookahead_ms == m.ceiling.lookahead_ms);
    assert(m2.ceiling.attack_ms == m.ceiling.attack_ms);
    assert(m2.ceiling.release_ms == m.ceiling.release_ms);
    std::fprintf(stderr, "[full] PASS (round-trip exact, deterministic)\n");
}

}  // namespace

int main() {
    test_valid_lookahead_loads();
    test_zero_lookahead_ok();
    test_negative_lookahead_throws();
    test_default_lookahead_ok();
    test_missing_file_throws();
    test_malformed_json_throws();
    test_unsupported_schema_version_throws();
    test_empty_class_order_throws();
    test_default_class_not_in_classes_throws();
    test_class_order_entry_not_in_classes_throws();
    test_controls_full_and_defaulted();
    test_ssl_comp_section_optional();
    test_ceiling_defaults();
    test_full_fixture_roundtrip_and_determinism();
    std::fprintf(stderr, "ALL 14 TESTS PASSED\n");
    return 0;
}
