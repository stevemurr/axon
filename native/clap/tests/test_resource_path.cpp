// test_resource_path — unit tests for the cross-platform discovery seams the
// Windows/Linux port rides on:
//
//   src/resource_path.hpp
//     - module_path_from_addr: the real per-OS API (dladdr on POSIX,
//       GetModuleHandleExW+GetModuleFileNameW on Windows) run against THIS
//       test binary, so every platform's branch is executed by its own CI.
//     - resources_dir_macos_layout / resources_dir_probe_layout: both layout
//       rules are plain inline functions compiled on every platform, so the
//       mac suite covers the Linux/Windows probe logic and vice versa. The
//       probe tests build real directory trees under a scratch dir.
//
//   src/ort_path.hpp
//     - ort_model_path: on POSIX it must return the SAME object (the mac
//       byte-identity argument is that .c_str() yields the identical pointer
//       the Ort::Session call sites passed before the port); on Windows it
//       must produce the UTF-16 form of the UTF-8 input.
//
// Build: registered as test_resource_path in native/clap/CMakeLists.txt.

#include "../src/ort_path.hpp"
#include "../src/resource_path.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// A function whose address is guaranteed to live in this executable's module.
void anchor_in_this_module() {}

int checks = 0;
void ok(const char* name) {
    ++checks;
    std::printf("  ok: %s\n", name);
}

fs::path make_scratch_dir() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path d = fs::temp_directory_path() /
                 ("axon_test_resource_path_" + std::to_string(nonce));
    fs::create_directories(d);
    return d;
}

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    assert(f.is_open());
    f << "{}";
}

// ---------------------------------------------------------------------------
// module_path_from_addr — exercises the real OS API on every platform.
// ---------------------------------------------------------------------------
void test_module_path_from_addr() {
    const std::string mod = nablafx::module_path_from_addr(
        reinterpret_cast<const void*>(&anchor_in_this_module));
    assert(!mod.empty() && "module_path_from_addr returned empty");
    assert(fs::exists(fs::path(mod)) && "module path does not exist on disk");
    const std::string base = fs::path(mod).filename().string();
    assert(base.find("test_resource_path") != std::string::npos &&
           "module path does not name this test binary");
    ok("module_path_from_addr resolves this test binary");
}

// ---------------------------------------------------------------------------
// resources_dir_macos_layout — pure path computation, no filesystem access.
// ---------------------------------------------------------------------------
void test_macos_layout() {
    const fs::path mod_dir =
        fs::path("plugins") / "Axon.clap" / "Contents" / "MacOS";
    const std::string got = nablafx::resources_dir_macos_layout(mod_dir);
    const std::string want =
        (fs::path("plugins") / "Axon.clap" / "Contents" / "Resources")
            .string();
    assert(got == want && "macOS layout must be <module_dir>/../Resources");
    ok("macOS layout: Contents/MacOS -> Contents/Resources");

    // No probing means it must not care whether the path exists.
    const std::string ghost = nablafx::resources_dir_macos_layout(
        fs::path("definitely") / "not" / "on" / "disk");
    assert(!ghost.empty() && "macOS layout must not probe the filesystem");
    ok("macOS layout: computed without existence checks (historical rule)");
}

// ---------------------------------------------------------------------------
// resources_dir_probe_layout — real directory trees under a scratch dir.
// ---------------------------------------------------------------------------
void test_probe_layout() {
    const fs::path scratch = make_scratch_dir();

    // 1. Directory bundle: <mod_dir>/Resources/<marker> wins.
    {
        const fs::path mod_dir = scratch / "bundle" / "Axon.clap";
        touch(mod_dir / "Resources" / "axon_meta.json");
        const std::string got =
            nablafx::resources_dir_probe_layout(mod_dir, "axon_meta.json");
        assert(got == (mod_dir / "Resources").string());
        ok("probe: directory bundle resolves to <mod_dir>/Resources");
    }

    // 2. Flat side-by-side: marker next to the module.
    {
        const fs::path mod_dir = scratch / "flat";
        touch(mod_dir / "axon_meta.json");
        const std::string got =
            nablafx::resources_dir_probe_layout(mod_dir, "axon_meta.json");
        assert(got == mod_dir.string());
        ok("probe: flat side-by-side resolves to <mod_dir>");
    }

    // 3. A stray Resources/ WITHOUT the marker must not shadow a flat
    //    install (the reason the probe requires the marker at all).
    {
        const fs::path mod_dir = scratch / "shadow";
        fs::create_directories(mod_dir / "Resources");  // no marker inside
        touch(mod_dir / "Resources" / "unrelated.txt");
        touch(mod_dir / "axon_meta.json");
        const std::string got =
            nablafx::resources_dir_probe_layout(mod_dir, "axon_meta.json");
        assert(got == mod_dir.string() &&
               "marker-less Resources/ must not shadow the flat layout");
        ok("probe: marker-less Resources/ cannot shadow a flat install");
    }

    // 4. When both layouts carry the marker, the directory bundle wins
    //    (documented probe order).
    {
        const fs::path mod_dir = scratch / "both";
        touch(mod_dir / "Resources" / "axon_meta.json");
        touch(mod_dir / "axon_meta.json");
        const std::string got =
            nablafx::resources_dir_probe_layout(mod_dir, "axon_meta.json");
        assert(got == (mod_dir / "Resources").string());
        ok("probe: bundle layout probed before flat");
    }

    // 5. Neither layout: empty string, not a throw.
    {
        const fs::path mod_dir = scratch / "nothing";
        fs::create_directories(mod_dir);
        const std::string got =
            nablafx::resources_dir_probe_layout(mod_dir, "axon_meta.json");
        assert(got.empty() && "no layout must yield empty string");
        // Also with a mod_dir that does not exist at all.
        const std::string got2 = nablafx::resources_dir_probe_layout(
            scratch / "never_created", "axon_meta.json");
        assert(got2.empty());
        ok("probe: no marker anywhere -> empty (no throw)");
    }

    // 6. The wrong marker must not match (single-model vs composite).
    {
        const fs::path mod_dir = scratch / "wrong_marker";
        touch(mod_dir / "Resources" / "plugin_meta.json");
        const std::string got =
            nablafx::resources_dir_probe_layout(mod_dir, "axon_meta.json");
        assert(got.empty());
        const std::string got_right =
            nablafx::resources_dir_probe_layout(mod_dir, "plugin_meta.json");
        assert(got_right == (mod_dir / "Resources").string());
        ok("probe: marker filename is per-plugin, not any-json");
    }

    std::error_code ec;
    fs::remove_all(scratch, ec);
}

// ---------------------------------------------------------------------------
// ort_model_path — the ORTCHAR_T boundary (src/ort_path.hpp).
// ---------------------------------------------------------------------------
void test_ort_model_path() {
    const std::string p = "weights/axon_bundle/ssl_comp/model.onnx";
#ifdef _WIN32
    const std::wstring wide = nablafx::ort_model_path(p);
    assert(wide == L"weights/axon_bundle/ssl_comp/model.onnx");
    ok("ort_model_path: ASCII converts 1:1 to UTF-16");

    // Non-ASCII: "modèle" — è is U+00E8 (UTF-8 0xC3 0xA8).
    const std::string utf8 = "mod\xC3\xA8le.onnx";
    const std::wstring w2 = nablafx::ort_model_path(utf8);
    assert(w2.size() == 11);  // one code unit per character
    assert(w2[3] == L'\x00E8' && "UTF-8 -> UTF-16 conversion wrong");
    ok("ort_model_path: UTF-8 multibyte -> single UTF-16 code unit");

    assert(nablafx::ort_model_path(std::string()).empty());
    ok("ort_model_path: empty in, empty out");
#else
    // POSIX contract: same OBJECT back (identical .c_str() pointer as the
    // pre-port call sites — the mac byte-identity argument).
    const std::string& r = nablafx::ort_model_path(p);
    assert(&r == &p && "POSIX ort_model_path must be a pass-through reference");
    assert(r.c_str() == p.c_str());
    ok("ort_model_path: POSIX identity pass-through (same object)");
#endif
}

}  // namespace

int main() {
    std::printf("test_resource_path:\n");
    test_module_path_from_addr();
    test_macos_layout();
    test_probe_layout();
    test_ort_model_path();
    std::printf("ALL RESOURCE-PATH TESTS PASSED (%d checks)\n", checks);
    return 0;
}
