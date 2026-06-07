// axon_gui_js.hpp
// Header-only helpers for building the JavaScript bridge strings sent to the
// WKWebView from axon_gui.mm. Kept Cocoa-free so the string-construction logic
// (the part that historically overflowed a fixed-size buffer) is unit-testable
// in isolation, with NO external dependencies.
//
// IMPORTANT: the output format here MUST stay byte-identical to what the GUI
// expects on the JS side (axonSetParam(<json-string-id>,<%g value>);).

#pragma once

#include <cstdio>
#include <string>

namespace axon {

// JSON-escape a C string into a double-quoted JS/JSON string literal.
inline std::string json_escape(const char* s) {
    std::string out;
    out.reserve(64);
    out += '"';
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += *p;     break;
        }
    }
    out += '"';
    return out;
}

// Build the "axonSetParam(<id>,<value>);" JS call.
//
// The param id is JSON-escaped (so it may be arbitrarily long) and the value is
// formatted with "%g" into a small bounded local buffer (a %g double cannot
// exceed ~25 chars). The whole call is assembled by std::string concatenation,
// so there is NO fixed-size destination buffer that the (unbounded) escaped id
// could overflow.
inline std::string build_set_param_js(const char* param_id, float value) {
    char valbuf[32];
    std::snprintf(valbuf, sizeof(valbuf), "%g", (double)value);
    return "axonSetParam(" + json_escape(param_id) + "," + valbuf + ");";
}

}  // namespace axon
