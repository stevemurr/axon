// axon_gui_bridge.hpp
// Platform-independent core of the Axon GUI <-> WebView bridge, shared by all
// three native backends (axon_gui.mm / axon_gui_gtk.cpp / axon_gui_win.cpp).
// Header-only, NO platform includes — every function here is pure string /
// data-structure logic so it is unit-testable on every platform
// (tests/test_axon_gui_bridge.cpp runs in the mac, Linux AND Windows suites).
//
// Three responsibilities:
//
//  1. build_init_js() — the axonInit({...}) handshake payload. Moved VERBATIM
//     from axon_gui.mm (Phase 3 of the Windows/Linux port) so the three
//     backends cannot drift; the byte format is pinned by the unit test.
//
//  2. decode_bridge_message() — the JS -> native message decoder. The page
//     posts JS objects:   { type: 'setParam', id: <string>, value: <number> }
//                         { type: 'reorder', order: [<int>...] }
//     macOS receives them as NSDictionary (WKWebView deserializes); the GTK
//     backend receives a JSCValue it serializes with jsc_value_to_json(); the
//     WebView2 backend injects a window.webkit.messageHandlers.axon shim that
//     posts JSON.stringify(m). Both non-mac backends therefore hold a JSON
//     string, and this ONE decoder (a minimal, dependency-free JSON parser)
//     is the single place that interprets it.
//
//  3. file_url_from_path() — absolute OS path -> file:// URL for the WebView
//     navigation call (webkit_web_view_load_uri / ICoreWebView2::Navigate).
//     Handles POSIX paths, Windows drive paths (either slash direction) and
//     UNC shares, with RFC 3986 percent-encoding.
//
// IMPORTANT: the strings produced here are part of the JS bridge CONTRACT
// with ui/index.html (axonInit / axonSetParam / messageHandlers.axon). Do not
// change formats without updating the page and every backend together.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "axon_gui.h"     // AxonParamInfo (pure C struct)
#include "axon_gui_js.hpp" // axon::json_escape

namespace axon {
namespace gui {

// ---------------------------------------------------------------------------
// 1. axonInit(...) payload builder (moved verbatim from axon_gui.mm).
// ---------------------------------------------------------------------------

// Build the full initial-state handshake:
//   axonInit({"paramMeta":{...},"paramValues":{...},"processorOrder":[...]});
inline std::string build_init_js(const AxonParamInfo* params, int n_params,
                                 const int* order, int order_count) {
    std::string js;
    js.reserve(2048);
    js += "axonInit({\"paramMeta\":{";

    for (int i = 0; i < n_params; ++i) {
        const AxonParamInfo& p = params[i];
        if (i) js += ',';
        js += json_escape(p.id);
        js += ":{\"name\":";
        js += json_escape(p.name);

        char buf[128];
        snprintf(buf, sizeof(buf),
                 ",\"min\":%g,\"max\":%g,\"def\":%g,\"unit\":",
                 (double)p.min, (double)p.max, (double)p.def);
        js += buf;
        js += json_escape(p.unit);

        if (p.enum_options && p.n_enum_options > 0) {
            js += ",\"enumOptions\":[";
            for (int k = 0; k < p.n_enum_options; ++k) {
                if (k) js += ',';
                js += json_escape(p.enum_options[k] ? p.enum_options[k] : "");
            }
            js += ']';
        }
        js += '}';
    }

    js += "},\"paramValues\":{";
    for (int i = 0; i < n_params; ++i) {
        const AxonParamInfo& p = params[i];
        if (i) js += ',';
        js += json_escape(p.id);
        char buf[64];
        snprintf(buf, sizeof(buf), ":%g", (double)p.current_value);
        js += buf;
    }

    js += "},\"processorOrder\":[";
    for (int i = 0; i < order_count; ++i) {
        if (i) js += ',';
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", order[i]);
        js += buf;
    }
    js += "]});";
    return js;
}

// ---------------------------------------------------------------------------
// 2. JS -> native bridge-message decoder.
// ---------------------------------------------------------------------------

struct BridgeMessage {
    enum class Kind { Invalid, SetParam, Reorder };
    Kind             kind  = Kind::Invalid;
    std::string      id;          // SetParam
    double           value = 0.0; // SetParam
    std::vector<int> order;       // Reorder
};

namespace detail {

inline void skip_ws(const char*& p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}

// Append a Unicode code point as UTF-8.
inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

inline bool parse_hex4(const char*& p, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = *p;
        v <<= 4;
        if (c >= '0' && c <= '9')       v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')  v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  v |= (uint32_t)(c - 'A' + 10);
        else return false;
        ++p;
    }
    *out = v;
    return true;
}

// Parse a JSON string literal (cursor on the opening '"'). Decodes escapes,
// including \uXXXX (with surrogate pairs) to UTF-8.
inline bool parse_string(const char*& p, std::string* out) {
    if (*p != '"') return false;
    ++p;
    std::string s;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            switch (*p) {
                case '"':  s += '"';  ++p; break;
                case '\\': s += '\\'; ++p; break;
                case '/':  s += '/';  ++p; break;
                case 'b':  s += '\b'; ++p; break;
                case 'f':  s += '\f'; ++p; break;
                case 'n':  s += '\n'; ++p; break;
                case 'r':  s += '\r'; ++p; break;
                case 't':  s += '\t'; ++p; break;
                case 'u': {
                    ++p;
                    uint32_t cp;
                    if (!parse_hex4(p, &cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate
                        if (p[0] != '\\' || p[1] != 'u') return false;
                        p += 2;
                        uint32_t lo;
                        if (!parse_hex4(p, &lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;  // lone low surrogate
                    }
                    append_utf8(s, cp);
                    break;
                }
                default: return false;
            }
        } else {
            s += *p++;
        }
    }
    if (*p != '"') return false;  // unterminated
    ++p;
    if (out) *out = std::move(s);
    return true;
}

inline bool parse_number(const char*& p, double* out) {
    // Validate the JSON number grammar start so strtod can't accept
    // extensions like "inf"/"nan"/hex.
    const char* start = p;
    if (*p == '-') ++p;
    if (!(*p >= '0' && *p <= '9')) return false;
    char* end = nullptr;
    double v = std::strtod(start, &end);
    if (end == start) return false;
    p = end;
    if (out) *out = v;
    return true;
}

inline bool skip_value(const char*& p, int depth);  // fwd

inline bool skip_object_or_array(const char*& p, char open, char close,
                                 bool is_object, int depth) {
    if (depth <= 0) return false;
    if (*p != open) return false;
    ++p;
    skip_ws(p);
    if (*p == close) { ++p; return true; }
    for (;;) {
        skip_ws(p);
        if (is_object) {
            if (!parse_string(p, nullptr)) return false;
            skip_ws(p);
            if (*p != ':') return false;
            ++p;
            skip_ws(p);
        }
        if (!skip_value(p, depth - 1)) return false;
        skip_ws(p);
        if (*p == ',') { ++p; continue; }
        if (*p == close) { ++p; return true; }
        return false;
    }
}

// Skip any JSON value (used for keys the bridge does not care about).
inline bool skip_value(const char*& p, int depth) {
    skip_ws(p);
    switch (*p) {
        case '"': return parse_string(p, nullptr);
        case '{': return skip_object_or_array(p, '{', '}', true, depth);
        case '[': return skip_object_or_array(p, '[', ']', false, depth);
        case 't': if (std::strncmp(p, "true", 4) == 0)  { p += 4; return true; } return false;
        case 'f': if (std::strncmp(p, "false", 5) == 0) { p += 5; return true; } return false;
        case 'n': if (std::strncmp(p, "null", 4) == 0)  { p += 4; return true; } return false;
        default:  return parse_number(p, nullptr);
    }
}

// Parse an array of numbers into ints (fractional parts truncated, matching
// the macOS backend's -[NSNumber intValue] behavior).
inline bool parse_int_array(const char*& p, std::vector<int>* out) {
    if (*p != '[') return false;
    ++p;
    std::vector<int> v;
    skip_ws(p);
    if (*p == ']') { ++p; *out = std::move(v); return true; }
    for (;;) {
        skip_ws(p);
        double d;
        if (!parse_number(p, &d)) return false;
        v.push_back((int)d);
        skip_ws(p);
        if (*p == ',') { ++p; continue; }
        if (*p == ']') { ++p; *out = std::move(v); return true; }
        return false;
    }
}

}  // namespace detail

// Decode one bridge message (a single JSON object). Returns true and fills
// `out` iff the message is one of the two well-formed bridge shapes; any
// parse error, missing field or unknown type yields false (kind = Invalid).
// Unknown extra keys are skipped, key order does not matter, and the last
// occurrence of a duplicated key wins (like JSON.parse / NSDictionary).
inline bool decode_bridge_message(const char* json, BridgeMessage& out) {
    using namespace detail;
    out = BridgeMessage{};
    if (!json) return false;

    const char* p = json;
    skip_ws(p);
    if (*p != '{') return false;
    ++p;

    std::string type, id;
    double value = 0.0;
    std::vector<int> order;
    bool have_type = false, have_id = false, have_value = false,
         have_order = false;

    skip_ws(p);
    if (*p == '}') {
        ++p;
    } else {
        for (;;) {
            skip_ws(p);
            std::string key;
            if (!parse_string(p, &key)) return false;
            skip_ws(p);
            if (*p != ':') return false;
            ++p;
            skip_ws(p);

            if (key == "type") {
                if (!parse_string(p, &type)) return false;
                have_type = true;
            } else if (key == "id") {
                if (!parse_string(p, &id)) return false;
                have_id = true;
            } else if (key == "value") {
                if (!parse_number(p, &value)) return false;
                have_value = true;
            } else if (key == "order") {
                if (!parse_int_array(p, &order)) return false;
                have_order = true;
            } else {
                if (!skip_value(p, /*depth=*/16)) return false;
            }

            skip_ws(p);
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; break; }
            return false;
        }
    }
    skip_ws(p);
    if (*p != '\0') return false;  // trailing garbage

    if (have_type && type == "setParam" && have_id && have_value) {
        out.kind  = BridgeMessage::Kind::SetParam;
        out.id    = std::move(id);
        out.value = value;
        return true;
    }
    if (have_type && type == "reorder" && have_order) {
        out.kind  = BridgeMessage::Kind::Reorder;
        out.order = std::move(order);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 3. Absolute OS path -> file:// URL.
// ---------------------------------------------------------------------------

namespace detail {

inline bool is_url_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '~';
}

inline void append_pct_encoded(std::string& out, const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : in) {
        if (is_url_unreserved(c) || c == '/') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
}

}  // namespace detail

// Convert an absolute filesystem path to a file:// URL.
//   POSIX     /a/b c        -> file:///a/b%20c
//   Windows   C:\a\b c      -> file:///C:/a/b%20c   (either slash direction)
//   UNC       \\srv\share\x -> file://srv/share/x
// Non-unreserved characters are percent-encoded as UTF-8 bytes ('/' kept).
// Purely deterministic string logic (no filesystem access) so it is
// unit-testable identically on every platform.
inline std::string file_url_from_path(const std::string& path) {
    std::string p = path;
    // Windows-style separators -> URL separators.
    for (char& c : p) {
        if (c == '\\') c = '/';
    }

    std::string url = "file://";
    if (p.size() >= 2 && p[0] == '/' && p[1] == '/') {
        // UNC //server/share/...  -> file://server/share/...
        size_t host_end = p.find('/', 2);
        std::string host = (host_end == std::string::npos)
                               ? p.substr(2)
                               : p.substr(2, host_end - 2);
        url += host;
        if (host_end != std::string::npos)
            detail::append_pct_encoded(url, p.substr(host_end));
        return url;
    }
    if (p.size() >= 2 && p[1] == ':' &&
        ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) {
        // Drive path C:/...  -> file:///C:/...
        url += '/';
        url += p[0];
        url += ':';
        detail::append_pct_encoded(url, p.substr(2));
        return url;
    }
    // POSIX absolute (or relative — caller passes absolute paths).
    detail::append_pct_encoded(url, p);
    return url;
}

// ---------------------------------------------------------------------------
// WebView2 bootstrap shim (Windows backend only, but kept here so its shape
// is unit-tested everywhere): ui/index.html posts through
// window.webkit.messageHandlers.axon.postMessage(obj). WKWebView and
// webkit2gtk provide that API natively; WebView2 does not, so this script —
// injected via AddScriptToExecuteOnDocumentCreated, i.e. before any page
// script runs — maps it onto window.chrome.webview.postMessage with the
// object pre-serialized to the JSON string decode_bridge_message() expects.
// ---------------------------------------------------------------------------
inline const char* webview2_bootstrap_js() {
    return "(function(){"
           "if(!window.webkit){window.webkit={};}"
           "if(!window.webkit.messageHandlers){window.webkit.messageHandlers={};}"
           "window.webkit.messageHandlers.axon={postMessage:function(m){"
           "window.chrome.webview.postMessage(JSON.stringify(m));}};"
           "})();";
}

}  // namespace gui
}  // namespace axon
