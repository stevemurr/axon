// Unit tests for the WKWebView JS-bridge string builders (axon_gui_js.hpp).
//
// REGRESSION: axon_gui_notify_param() used to format the whole JS call into a
// fixed-size `char buf[256]` via snprintf(). A long (or maliciously crafted)
// param id — once JSON-escaped — overflowed/truncated that buffer. The fix
// builds the call by std::string concatenation, with only the "%g" value going
// into a small bounded local buffer. These tests exercise that previously-broken
// path: ids far longer than 256 bytes must produce the FULL, untruncated call.
//
//   g++ -O2 -std=c++17 -I src tests/test_axon_gui_js.cpp -o tests/test_axon_gui_js \
//       && tests/test_axon_gui_js
//
#include "../src/axon_gui_js.hpp"

#include <cassert>
#include <cstdio>
#include <string>

namespace {

// Reference builder: what the JS string is *supposed* to be, derived directly
// from the documented format (axonSetParam(<json-id>,<%g value>);) without any
// fixed-size buffer. Used as the source of truth the implementation must match.
std::string expected_call(const std::string& escaped_id, float value) {
    char vb[64];
    std::snprintf(vb, sizeof(vb), "%g", (double)value);
    return "axonSetParam(" + escaped_id + "," + std::string(vb) + ");";
}

// ---------------------------------------------------------------------------
// Test 1: json_escape — every special char is escaped, plain chars pass through,
//         and the result is wrapped in double quotes.
// ---------------------------------------------------------------------------
void test_json_escape_basic() {
    assert(axon::json_escape("foo") == "\"foo\"");
    assert(axon::json_escape("") == "\"\"");
    assert(axon::json_escape("a\"b") == "\"a\\\"b\"");
    assert(axon::json_escape("a\\b") == "\"a\\\\b\"");
    assert(axon::json_escape("a\nb") == "\"a\\nb\"");
    assert(axon::json_escape("a\rb") == "\"a\\rb\"");
    assert(axon::json_escape("a\tb") == "\"a\\tb\"");
    // Mixed: a quote, a backslash and a newline together.
    assert(axon::json_escape("x\"\\\n") == "\"x\\\"\\\\\\n\"");
    std::fprintf(stderr, "[escape] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: format — short, ordinary ids produce the exact expected JS call,
//         and the numeric "%g" rendering is preserved identically.
// ---------------------------------------------------------------------------
void test_format_basic() {
    assert(axon::build_set_param_js("gain", 0.5f) ==
           "axonSetParam(\"gain\",0.5);");
    assert(axon::build_set_param_js("width", 2.0f) ==
           "axonSetParam(\"width\",2);");
    assert(axon::build_set_param_js("mix", 0.0f) ==
           "axonSetParam(\"mix\",0);");
    // A param id that itself needs escaping must be escaped inside the call.
    assert(axon::build_set_param_js("a\"b", 1.0f) ==
           "axonSetParam(\"a\\\"b\",1);");
    std::fprintf(stderr, "[format] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: numeric "%g" parity — a sweep of values must render byte-for-byte the
//         same as snprintf("%g"), confirming the value path is unchanged by the
//         refactor (the fix preserves numeric output identically).
// ---------------------------------------------------------------------------
void test_numeric_parity() {
    const float vals[] = {0.f, 1.f, -1.f, 0.5f, -12345.678f, 1e-9f, 1e9f,
                          3.14159f, 1234567.f, -0.000123f};
    for (float v : vals) {
        std::string got = axon::build_set_param_js("p", v);
        std::string want = expected_call("\"p\"", v);
        assert(got == want);
    }
    std::fprintf(stderr, "[numeric] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: LONG ID (the headline regression). An id far longer than the old
//         256-byte buffer must yield the COMPLETE, untruncated call. With the
//         old `char buf[256]` + snprintf, the JS would be cut off mid-id and the
//         trailing ",<value>);" would be lost — this asserts that does NOT happen.
// ---------------------------------------------------------------------------
void test_long_id_no_truncation() {
    for (int len : {200, 256, 257, 512, 4096}) {
        std::string id(len, 'p');           // plain chars, len > old buffer
        std::string got = axon::build_set_param_js(id.c_str(), 0.5f);
        std::string want = "axonSetParam(\"" + id + "\",0.5);";

        // Full call survives: nothing truncated.
        assert(got == want);
        // The id appears in full (length-exact), and the call is well-formed.
        assert(got.size() == want.size());
        assert(got.rfind("\",0.5);") == got.size() - 7);   // ends correctly
        assert(got.find("axonSetParam(\"") == 0);          // starts correctly
        // Sanity: total length exceeds the old fixed buffer for the big cases.
        if (len >= 256) assert(got.size() > 256);
    }
    std::fprintf(stderr, "[longid] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: LONG ID WITH ESCAPES — escaping inflates the byte count beyond the id
//         length, so even a sub-256 id can blow past 256 once escaped. Every
//         quote/backslash must still be escaped and the value appended intact.
// ---------------------------------------------------------------------------
void test_long_escaped_id() {
    // 300 quote chars → each becomes \" (2 bytes) → 600 bytes inside the string.
    std::string id(300, '"');
    std::string got = axon::build_set_param_js(id.c_str(), -3.5f);

    std::string esc = "\"";                  // opening quote of the JS string
    for (char c : id) esc += "\\\"";         // every '"' escaped
    esc += "\"";                             // closing quote
    std::string want = "axonSetParam(" + esc + ",-3.5);";

    assert(got == want);
    assert(got.size() > 256);                // would have overflowed the old buf
    // No raw, unescaped quote may appear between the opening and closing quotes
    // of the id literal (i.e. the body is fully escaped).
    // Count backslash-quote pairs == 300.
    int pairs = 0;
    for (size_t i = 0; i + 1 < got.size(); ++i)
        if (got[i] == '\\' && got[i + 1] == '"') ++pairs;
    assert(pairs == 300);
    std::fprintf(stderr, "[escid] PASS\n");
}

}  // namespace

int main() {
    test_json_escape_basic();
    test_format_basic();
    test_numeric_parity();
    test_long_id_no_truncation();
    test_long_escaped_id();
    std::fprintf(stderr, "ALL 5 TESTS PASSED\n");
    return 0;
}
