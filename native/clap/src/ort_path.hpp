// ort_path.hpp — ORTCHAR_T-aware model-path helper for the two Ort::Session
// construction sites (src/ort_session.cpp, src/axon_plugin.cpp).
//
// Ort::Session's path parameter is `const ORTCHAR_T*`: `char` (UTF-8) on
// POSIX but `wchar_t` (UTF-16) on Windows. The repo carries model paths as
// UTF-8 std::string; this helper converts exactly at the API boundary.
//
//   std::make_unique<Ort::Session>(env, ort_model_path(path).c_str(), opts);
//
// On macOS/Linux ort_model_path() returns the same std::string by const
// reference — .c_str() yields the identical pointer the call sites passed
// before, so mac behavior is unchanged by construction. On Windows it
// returns a std::wstring converted via MultiByteToWideChar(CP_UTF8, ...);
// the temporary lives to the end of the full expression, which outlives the
// Session constructor call.

#pragma once

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nablafx {

#ifdef _WIN32
inline std::wstring ort_model_path(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                      static_cast<int>(utf8.size()),
                                      nullptr, 0);
    std::wstring wide(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                            static_cast<int>(utf8.size()), &wide[0], n);
    return wide;
}
#else
inline const std::string& ort_model_path(const std::string& utf8) {
    return utf8;
}
#endif

}  // namespace nablafx
