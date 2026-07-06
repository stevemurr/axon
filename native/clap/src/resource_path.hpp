// resource_path.hpp — cross-platform plugin resource discovery.
//
// The plugins locate their model bundles + ui/ relative to the shared object
// they are running from. This header is the single place that knows (a) how
// to resolve the running module's path on each OS and (b) what the on-disk
// resource layout is per platform.
//
// LAYOUTS (the port's resource convention — see
// docs/future/active/windows-linux-builds.md, "Implementation notes"):
//
//   macOS (unchanged, byte-identical behavior):
//       Axon.clap/                        <- bundle directory
//         Contents/MacOS/Axon             <- the dylib (module path)
//         Contents/Resources/...          <- resources root
//     The resources root is ALWAYS <module_dir>/../Resources. No probing —
//     exactly the historical behavior (no existence check either; a missing
//     meta file fails at load, as before).
//
//   Linux / Windows (single-file .clap = ELF .so / PE DLL):
//     1. DIRECTORY BUNDLE (what our packaging scripts produce):
//          Axon.clap/                     <- plain directory
//            Axon.so   (or Axon.clap)     <- the module
//            libonnxruntime.so.*          <- rpath $ORIGIN
//            Resources/axon_meta.json     <- resources root = Axon.clap/Resources
//            Resources/ui/index.html ...
//     2. FLAT SIDE-BY-SIDE (hand-installed bare .so):
//          <plugins-dir>/Axon.clap        <- the module
//          <plugins-dir>/axon_meta.json   <- resources root = <plugins-dir>
//     Probe order: <module_dir>/Resources first (must contain the marker
//     file), then <module_dir> itself. The marker file is the plugin's meta
//     json (axon_meta.json / plugin_meta.json) so an unrelated Resources/
//     directory can't shadow the real layout. Returns "" if neither matches.
//
// Windows note (Phase 2): GetModuleFileNameW on the HMODULE resolved from an
// address in this module via GetModuleHandleExW; UTF-16 -> UTF-8 conversion
// mirrors ort_path.hpp's boundary conversion in reverse.

#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nablafx {

// Absolute path of the shared object / DLL that contains `addr_in_module`
// (pass the address of any function or static defined in the plugin).
// Returns "" on failure.
inline std::string module_path_from_addr(const void* addr_in_module) {
#ifdef _WIN32
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr_in_module), &mod) ||
        !mod) {
        return {};
    }
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const int u8 = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                       nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(u8 > 0 ? u8 : 0), '\0');
    if (u8 > 0)
        WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), &out[0], u8,
                            nullptr, nullptr);
    return out;
#else
    Dl_info info{};
    if (dladdr(addr_in_module, &info) == 0 || !info.dli_fname) {
        return {};
    }
    return std::string(info.dli_fname);
#endif
}

// Resolve the plugin's resources directory (see layout comment above).
// `marker` is the meta file that must exist at the resources root on
// non-Apple platforms (e.g. "axon_meta.json"). Returns "" on failure.
inline std::string find_resources_dir(const void* addr_in_module,
                                      const char* marker) {
    const std::string mod = module_path_from_addr(addr_in_module);
    if (mod.empty()) return {};
    const std::filesystem::path mod_dir =
        std::filesystem::path(mod).parent_path();
#ifdef __APPLE__
    // .clap/Contents/MacOS/<dylib> -> .clap/Contents/Resources. Historical
    // behavior, kept verbatim: no marker probing, no existence check.
    (void)marker;
    return (mod_dir.parent_path() / "Resources").string();
#else
    std::error_code ec;
    const std::filesystem::path candidates[] = {
        mod_dir / "Resources",  // directory bundle
        mod_dir,                // flat side-by-side
    };
    for (const auto& dir : candidates) {
        if (std::filesystem::is_regular_file(dir / marker, ec)) {
            return dir.string();
        }
    }
    return {};
#endif
}

}  // namespace nablafx
