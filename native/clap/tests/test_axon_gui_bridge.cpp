// Unit tests for the shared GUI bridge core (src/axon_gui_bridge.hpp): the
// axonInit payload builder, the JS->native bridge-message decoder used by the
// webkit2gtk and WebView2 backends, and the file:// URL builder. All pure
// logic — this test runs identically on macOS, Linux and Windows, so a bridge
// regression shows up on every platform's suite, not just in platform CI.
//
//   g++ -O2 -std=c++17 -I src tests/test_axon_gui_bridge.cpp \
//       -o tests/test_axon_gui_bridge && tests/test_axon_gui_bridge

#include "../src/axon_gui_bridge.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using axon::gui::BridgeMessage;
using axon::gui::build_init_js;
using axon::gui::decode_bridge_message;
using axon::gui::file_url_from_path;

namespace {

// ---------------------------------------------------------------------------
// build_init_js — the axonInit({...}) handshake. GOLDEN bytes: this payload
// format is the contract with ui/index.html's axonInit() and was produced by
// axon_gui.mm since the GUI shipped; the builder moved to the shared header
// verbatim and must keep producing exactly these bytes.
// ---------------------------------------------------------------------------
void test_build_init_js_golden() {
    const char* cls_opts[] = {"vocal", "drums", "mix"};
    AxonParamInfo params[2] = {};
    params[0].id             = "AMT";
    params[0].name           = "Amount";
    params[0].min            = 0.f;
    params[0].max            = 1.f;
    params[0].def            = 0.5f;
    params[0].unit           = "%";
    params[0].current_value  = 0.25f;
    params[1].id             = "CLS";
    params[1].name           = "Class";
    params[1].min            = 0.f;
    params[1].max            = 2.f;
    params[1].def            = 0.f;
    params[1].unit           = "";
    params[1].current_value  = 1.f;
    params[1].enum_options   = cls_opts;
    params[1].n_enum_options = 3;
    const int order[3] = {3, 1, 2};

    const std::string got = build_init_js(params, 2, order, 3);
    const std::string want =
        "axonInit({\"paramMeta\":{"
        "\"AMT\":{\"name\":\"Amount\",\"min\":0,\"max\":1,\"def\":0.5,\"unit\":\"%\"},"
        "\"CLS\":{\"name\":\"Class\",\"min\":0,\"max\":2,\"def\":0,\"unit\":\"\","
        "\"enumOptions\":[\"vocal\",\"drums\",\"mix\"]}"
        "},\"paramValues\":{\"AMT\":0.25,\"CLS\":1},"
        "\"processorOrder\":[3,1,2]});";
    assert(got == want);
    std::fprintf(stderr, "[init-golden] PASS\n");
}

void test_build_init_js_edges() {
    // Empty everything: still a syntactically complete handshake.
    assert(build_init_js(nullptr, 0, nullptr, 0) ==
           "axonInit({\"paramMeta\":{},\"paramValues\":{},"
           "\"processorOrder\":[]});");

    // Ids/names/units that need JSON escaping get escaped (same json_escape
    // as axonSetParam — pinned in test_axon_gui_js.cpp).
    AxonParamInfo p = {};
    p.id            = "a\"b";
    p.name          = "line\nbreak";
    p.min           = -1.f;
    p.max           = 1.f;
    p.def           = 0.f;
    p.unit          = "d\\B";
    p.current_value = -0.5f;
    const int order[1] = {0};
    const std::string got = build_init_js(&p, 1, order, 1);
    const std::string want =
        "axonInit({\"paramMeta\":{"
        "\"a\\\"b\":{\"name\":\"line\\nbreak\",\"min\":-1,\"max\":1,"
        "\"def\":0,\"unit\":\"d\\\\B\"}"
        "},\"paramValues\":{\"a\\\"b\":-0.5},\"processorOrder\":[0]});";
    assert(got == want);

    // A NULL entry inside enum_options must not crash and renders as "".
    const char* opts[2] = {"one", nullptr};
    AxonParamInfo q = {};
    q.id = "E"; q.name = "E"; q.unit = "";
    q.enum_options = opts; q.n_enum_options = 2;
    const std::string with_null = build_init_js(&q, 1, order, 1);
    assert(with_null.find("\"enumOptions\":[\"one\",\"\"]") != std::string::npos);
    std::fprintf(stderr, "[init-edges] PASS\n");
}

// ---------------------------------------------------------------------------
// decode_bridge_message — happy paths. These are byte-for-byte the shapes
// ui/index.html sends: sendParam() posts {type:'setParam',id,value}, and
// sendOrderToHost() posts {type:'reorder',order:[...]}; on GTK they arrive
// via jsc_value_to_json (compact), on Windows via JSON.stringify (compact).
// ---------------------------------------------------------------------------
void test_decode_set_param() {
    BridgeMessage m;
    assert(decode_bridge_message(
        "{\"type\":\"setParam\",\"id\":\"AMT\",\"value\":0.42}", m));
    assert(m.kind == BridgeMessage::Kind::SetParam);
    assert(m.id == "AMT");
    assert(m.value > 0.419 && m.value < 0.421);

    // Key order must not matter (JSON objects are unordered).
    assert(decode_bridge_message(
        "{\"value\":1,\"id\":\"BYP\",\"type\":\"setParam\"}", m));
    assert(m.kind == BridgeMessage::Kind::SetParam);
    assert(m.id == "BYP" && m.value == 1.0);

    // Whitespace tolerated anywhere tokens allow it.
    assert(decode_bridge_message(
        " { \"type\" : \"setParam\" , \"id\" : \"X\" , \"value\" : -3 } ", m));
    assert(m.id == "X" && m.value == -3.0);

    // Number forms: integer, negative, exponent, fraction.
    assert(decode_bridge_message(
        "{\"type\":\"setParam\",\"id\":\"p\",\"value\":2e-3}", m));
    assert(m.value > 0.00199 && m.value < 0.00201);

    // Escapes in the id (quote, backslash, \uXXXX -> UTF-8).
    assert(decode_bridge_message(
        "{\"type\":\"setParam\",\"id\":\"a\\\"b\\\\c\",\"value\":0}", m));
    assert(m.id == "a\"b\\c");
    assert(decode_bridge_message(
        "{\"type\":\"setParam\",\"id\":\"gain\\u2192db\",\"value\":0}", m));
    assert(m.id == "gain\xE2\x86\x92""db");  // U+2192 as UTF-8

    // Unknown extra keys (any JSON value, nested included) are skipped.
    assert(decode_bridge_message(
        "{\"type\":\"setParam\",\"junk\":{\"a\":[1,{\"b\":null}],\"c\":true},"
        "\"id\":\"K\",\"value\":7,\"more\":\"x\"}", m));
    assert(m.id == "K" && m.value == 7.0);
    std::fprintf(stderr, "[decode-setparam] PASS\n");
}

void test_decode_reorder() {
    BridgeMessage m;
    assert(decode_bridge_message(
        "{\"type\":\"reorder\",\"order\":[6,3,1,8,9,4,5]}", m));
    assert(m.kind == BridgeMessage::Kind::Reorder);
    assert((m.order == std::vector<int>{6, 3, 1, 8, 9, 4, 5}));

    // Empty order array is structurally valid (count 0, like the mac path).
    assert(decode_bridge_message("{\"type\":\"reorder\",\"order\":[]}", m));
    assert(m.order.empty());

    // Fractional entries truncate toward zero (NSNumber intValue parity).
    assert(decode_bridge_message(
        "{\"type\":\"reorder\",\"order\":[1.9,-2.9,3]}", m));
    assert((m.order == std::vector<int>{1, -2, 3}));
    std::fprintf(stderr, "[decode-reorder] PASS\n");
}

// ---------------------------------------------------------------------------
// decode_bridge_message — every malformed shape must return false and leave
// kind == Invalid (the backends drop the message; they must never crash or
// half-apply).
// ---------------------------------------------------------------------------
void test_decode_rejects() {
    BridgeMessage m;
    const char* bad[] = {
        "",                                                    // empty
        "   ",                                                 // ws only
        "null",                                                // not an object
        "42",                                                  // not an object
        "\"setParam\"",                                        // not an object
        "[1,2,3]",                                             // array root
        "{",                                                   // truncated
        "{\"type\":\"setParam\",\"id\":\"A\",\"value\":}",     // missing value
        "{\"type\":\"setParam\",\"id\":\"A\"}",                // no value key
        "{\"type\":\"setParam\",\"value\":1}",                 // no id key
        "{\"id\":\"A\",\"value\":1}",                          // no type key
        "{\"type\":\"unknown\",\"id\":\"A\",\"value\":1}",     // unknown type
        "{\"type\":\"reorder\"}",                              // no order key
        "{\"type\":\"reorder\",\"order\":3}",                  // order not array
        "{\"type\":\"reorder\",\"order\":[\"a\"]}",            // non-numeric
        "{\"type\":\"setParam\",\"id\":\"A\",\"value\":\"x\"}",// value not number
        "{\"type\":\"setParam\",\"id\":7,\"value\":1}",        // id not string
        "{\"type\":\"setParam\",\"id\":\"A\",\"value\":1}x",   // trailing junk
        "{\"type\":\"setParam\",\"id\":\"A\" \"value\":1}",    // missing comma
        "{\"type\":\"setParam\",\"id\":\"unterminated",        // bad string
        "{\"type\":\"setParam\",\"id\":\"\\u12\",\"value\":1}",// bad \u escape
        "{\"type\":\"setParam\",\"id\":\"\\uD800x\",\"value\":1}", // lone surrogate
        "{\"type\":\"setParam\",\"id\":\"A\",\"value\":nan}",  // non-JSON number
    };
    for (const char* s : bad) {
        bool ok = decode_bridge_message(s, m);
        if (ok) std::fprintf(stderr, "  accepted bad input: %s\n", s);
        assert(!ok);
        assert(m.kind == BridgeMessage::Kind::Invalid);
    }
    assert(!decode_bridge_message(nullptr, m));

    // Deeply nested unknown value beyond the skip depth limit: rejected
    // (bounded recursion), not a crash.
    std::string deep = "{\"type\":\"setParam\",\"junk\":";
    for (int i = 0; i < 64; ++i) deep += "[";
    for (int i = 0; i < 64; ++i) deep += "]";
    deep += ",\"id\":\"A\",\"value\":1}";
    assert(!decode_bridge_message(deep.c_str(), m));
    std::fprintf(stderr, "[decode-rejects] PASS\n");
}

// ---------------------------------------------------------------------------
// file_url_from_path — POSIX, Windows drive (both slash directions), UNC,
// percent-encoding of spaces/UTF-8/reserved chars.
// ---------------------------------------------------------------------------
void test_file_url() {
    // POSIX plain.
    assert(file_url_from_path("/usr/lib/axon/Resources/ui/index.html") ==
           "file:///usr/lib/axon/Resources/ui/index.html");
    // POSIX with spaces + percent + hash (all must be encoded).
    assert(file_url_from_path("/Users/me/My Plugins/50% off#1/ui.html") ==
           "file:///Users/me/My%20Plugins/50%25%20off%231/ui.html");
    // UTF-8 bytes are %-encoded byte-wise.
    assert(file_url_from_path("/tmp/\xC3\xA9") == "file:///tmp/%C3%A9");

    // Windows drive path, backslashes.
    assert(file_url_from_path("C:\\Users\\bob\\Axon.clap\\Resources") ==
           "file:///C:/Users/bob/Axon.clap/Resources");
    // Windows drive path, forward slashes + space.
    assert(file_url_from_path("c:/Program Files/Axon/ui/index.html") ==
           "file:///c:/Program%20Files/Axon/ui/index.html");

    // UNC share.
    assert(file_url_from_path("\\\\server\\share\\dir\\f.html") ==
           "file://server/share/dir/f.html");

    std::fprintf(stderr, "[file-url] PASS\n");
}

// ---------------------------------------------------------------------------
// WebView2 bootstrap shim: must define the exact API surface index.html
// probes for (window.webkit.messageHandlers.axon.postMessage) and forward
// through chrome.webview with JSON serialization.
// ---------------------------------------------------------------------------
void test_bootstrap_shim_tokens() {
    const std::string js = axon::gui::webview2_bootstrap_js();
    assert(js.find("window.webkit.messageHandlers.axon") != std::string::npos);
    assert(js.find("postMessage") != std::string::npos);
    assert(js.find("window.chrome.webview.postMessage") != std::string::npos);
    assert(js.find("JSON.stringify") != std::string::npos);
    std::fprintf(stderr, "[bootstrap] PASS\n");
}

// ---------------------------------------------------------------------------
// Round trip: what the shim would post for the page's two message shapes
// decodes to the same values (simulating the WebView2 path end-to-end at the
// string level).
// ---------------------------------------------------------------------------
void test_round_trip_shapes() {
    // JSON.stringify({type:'setParam',id:'SEQ_LF_G',value:-2.5}) output shape:
    BridgeMessage m;
    assert(decode_bridge_message(
        "{\"type\":\"setParam\",\"id\":\"SEQ_LF_G\",\"value\":-2.5}", m));
    assert(m.kind == BridgeMessage::Kind::SetParam);
    assert(m.id == "SEQ_LF_G" && m.value == -2.5);

    assert(decode_bridge_message("{\"type\":\"reorder\",\"order\":[0]}", m));
    assert(m.kind == BridgeMessage::Kind::Reorder);
    assert((m.order == std::vector<int>{0}));
    std::fprintf(stderr, "[round-trip] PASS\n");
}

}  // namespace

int main() {
    test_build_init_js_golden();
    test_build_init_js_edges();
    test_decode_set_param();
    test_decode_reorder();
    test_decode_rejects();
    test_file_url();
    test_bootstrap_shim_tokens();
    test_round_trip_shapes();
    std::fprintf(stderr, "ALL 8 TESTS PASSED\n");
    return 0;
}
