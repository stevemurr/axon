// dyn_module.hpp — minimal cross-platform dynamic-module loading, the
// companion seam to resource_path.hpp (which resolves paths of modules that
// are already loaded; this header loads them). Used by the headless CLAP
// host (bench/axon_bench.cpp); the plugins themselves never load modules.
//
//   * POSIX: dlopen(RTLD_NOW | RTLD_LOCAL) / dlsym / dlclose — exactly the
//     calls axon_bench made directly before the Windows port.
//   * Windows: LoadLibraryExW with LOAD_WITH_ALTERED_SEARCH_PATH, which makes
//     the loader resolve the plugin's OWN dependent DLLs (onnxruntime.dll,
//     staged next to the .clap by package_windows.py) from the plugin's
//     directory first instead of the host executable's. This mirrors what
//     real Windows CLAP/VST3 hosts do, and is the reason the Windows bundle
//     needs no rpath equivalent. LOAD_WITH_ALTERED_SEARCH_PATH requires an
//     absolute path, so dyn_open absolutizes; the UTF-8 -> UTF-16 boundary
//     conversion matches ort_path.hpp.
//
// dyn_sym returns data and function symbols alike (GetProcAddress serves
// both on Windows, dlsym on POSIX); axon_bench needs the DATA export
// `clap_entry`.

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

#include <filesystem>
#else
#include <dlfcn.h>
#endif

namespace nablafx {

#ifdef _WIN32

using DynModule = HMODULE;

inline DynModule dyn_open(const std::string& utf8_path) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path.data(),
                                      static_cast<int>(utf8_path.size()),
                                      nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.data(),
                        static_cast<int>(utf8_path.size()), &wide[0], n);
    // LOAD_WITH_ALTERED_SEARCH_PATH is only honored for absolute paths.
    std::error_code ec;
    std::filesystem::path abs =
        std::filesystem::absolute(std::filesystem::path(wide), ec);
    if (ec) return nullptr;
    return LoadLibraryExW(abs.c_str(), nullptr,
                          LOAD_WITH_ALTERED_SEARCH_PATH);
}

inline void* dyn_sym(DynModule mod, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(mod, name));
}

inline void dyn_close(DynModule mod) {
    if (mod) FreeLibrary(mod);
}

// Human-readable reason for the most recent dyn_open/dyn_sym failure.
inline std::string dyn_error() {
    const DWORD err = GetLastError();
    char buf[512] = {0};
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        err, 0, buf, static_cast<DWORD>(sizeof(buf) - 1), nullptr);
    if (n == 0) return "error " + std::to_string(err);
    std::string msg(buf, n);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n'))
        msg.pop_back();
    return msg + " (error " + std::to_string(err) + ")";
}

#else  // POSIX

using DynModule = void*;

inline DynModule dyn_open(const std::string& path) {
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

inline void* dyn_sym(DynModule mod, const char* name) {
    return dlsym(mod, name);
}

inline void dyn_close(DynModule mod) {
    if (mod) dlclose(mod);
}

inline std::string dyn_error() {
    const char* e = dlerror();
    return e ? std::string(e) : std::string("unknown dlopen/dlsym error");
}

#endif

}  // namespace nablafx
