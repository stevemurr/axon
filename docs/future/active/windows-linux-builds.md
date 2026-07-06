# Support Windows and Linux builds

Status: active
Opened: 2026-07-06
Issue: #23

Owner-requested. Investigation FIRST — the coupling to macOS runs deeper than
the CMake platform gate, so no code changes until the full cost is mapped.

## Why / evidence

- The plugin is currently macOS-arm64-only by explicit gate
  (native/clap/CMakeLists.txt), but the real coupling is layered: Apple
  Accelerate/vDSP throughout the DSP (FFTs and vector math), Darwin clocks in
  the timing tooling, a WebKit-hosted UI, prebuilt macOS ONNX Runtime
  binaries, codesigning, and mac-only install/bench plumbing.
- CLAP itself is cross-platform; ONNX Runtime ships official Windows/Linux
  binaries; the question is the size and shape of the porting surface, not
  feasibility.

## Plan

Read-only investigation: inventory every platform coupling with file:line,
per-subsystem options and effort, and a phased plan (likely: headless
Linux build of DSP + tests first; GUI and Windows after). Results appended
below when complete.

## Acceptance (for the investigation itself)

A Results section good enough that implementation can be scoped into
phases with confidence — each phase with its own acceptance criteria.

## Results (2026-07-06, read-only scoping investigation)

**Verdict: no research problem anywhere — but the owner's "more work than it
appears" instinct is right, and the surprise is WHERE the work is.** The
Accelerate dependency everyone would fear is small and cleanly boundable; the
real budget is GUI reimplementation and a resource/packaging redesign.

### The Accelerate surface is only ~17 symbols across 6 files

FFT lifecycle/transform (`vDSP_create/destroy_fftsetup`, `vDSP_fft_zrip` ×6,
`ctoz/ztoc`), complex helpers (`zvmul`, `zvmags`), real vector math
(`vmul` ×13, `vadd` ×8, `vsmul`, `vsma`, `vclr`, `svesq`, `dotpr`), and 3
vForce calls (`vvexpf/vvcosf/vvsinf`). All FFTs are power-of-two (1024 /
2048 / 4096). The load-bearing subtlety: multiple call sites manipulate
vDSP's **zrip packed format directly** (DC in `realp[0]`, Nyquist in
`imagp[0]` — mel_limiter gain apply, spectral_mask min-phase/cepstral
round-trips, coherence_distortion), so any replacement must reproduce that
packing.

**Recommended approach: a thin `accelerate_shim.hpp`, not a call-site
migration.** `#ifdef __APPLE__` → real Accelerate (mac renders stay
byte-identical, the repo's null-test discipline untouched); other platforms
get the same 17 symbols implemented over **pffft** (BSD-3 — FFTW is
GPL/paid, a non-starter) with a zrip repack layer, plus plain loops for the
vector/vForce ops. Every call site compiles unmodified everywhere.
Accelerate portability ≈ M total, isolated in one header.

### Other couplings (all bounded)

ORT ships official win-x64 / linux-x64 / linux-aarch64 prebuilts on the same
URL scheme (Windows needs the `ORTCHAR_T` wide-path at the two
`Ort::Session` sites). Darwin clock + `__builtin_clzll` in the timing header
→ `std::chrono` / `std::countl_zero`. Bench `dlopen` → `LoadLibrary` on
Windows. Toolchain recommendation for Windows: **clang-cl** (minimizes
divergence from the mac compiler). CLI grows `install --linux/--win`.
**Resource discovery is a real design task**: the plugin locates models +
`ui/` via the macOS `…/Contents/Resources` bundle layout
(`axon_plugin.cpp` dladdr path); Win/Linux `.clap`s are single binaries, so
a deliberate resource convention (dir-bundle or side-by-side) must touch
loading, packaging, and installers.

### GUI: cleanly severable, and the dominant cost

The entire Cocoa/WebKit surface is one 414-line `axon_gui.mm` behind a pure
C ABI; `ui/index.html` is portable and reused verbatim. Ports = one file per
platform: webkit2gtk/X11 on Linux (L), WebView2 on Windows (L/XL, runtime
bootstrap friction). Critically, **the GUI extension is optional** — all
params are exposed via CLAP params, so a headless plugin is fully usable
with hosts' generic UIs.

### Phased plan

- **Phase 0 (S–M)** — shims only; macOS build must remain byte-identical
  (the guardrail for the whole effort). Shim header + clock/intrinsics +
  ORT path helper + CMake platform branches.
- **Phase 1 (M)** — Linux x64 headless: ORT prebuilt, resource convention,
  full test suite + bench + steady-state tolerance tests in `ubuntu-latest`
  CI; loads in a Linux CLAP host.
- **Phase 2 (M)** — Windows x64 headless: clang-cl + Ninja on
  `windows-latest`; same acceptance.
- **Phase 3 (L → XL)** — GUIs: webkit2gtk backend, then WebView2, same C
  ABI and JS bridge.

**Acceptance caveat:** cross-platform builds CANNOT be null-tested against
mac renders (different FFT backend; two stages already amplify 1-ULP noise
to −75..−90 dBFS). The port needs its own per-platform oracle: unit suite +
steady-state magnitude-tolerance tests + listening — currently absent, part
of Phase 1.

### Effort + top risks

Headless Linux+Windows (Phases 0–2) = **Medium**. Full parity with both
GUIs = **XL** (GUI-dominated). Top risks: (1) the two native WebView
embeddings, (2) the resource/packaging redesign, (3) no bit-exact acceptance
oracle across platforms. Descope first: GUIs (ship headless) and
ARM targets (x64 first) — that converts XL → M.

## Implementation notes

### Phase 1 (2026-07-06, Linux x64 headless) — SHIPPED

**Resource convention (risk #2, decided).** One helper —
`src/resource_path.hpp` — is the single place that resolves the running
module's path (`dladdr` on POSIX, `GetModuleFileNameW` path ready for
Phase 2) and knows the per-platform layout:

- **macOS (unchanged, byte-identical):** resources root is always
  `<module_dir>/../Resources` (`Axon.clap/Contents/MacOS/<bin>` →
  `Contents/Resources`). No probing, no existence checks — the historical
  behavior verbatim.
- **Linux/Windows — two layouts, one probe rule.** The loader probes, in
  order: `<module_dir>/Resources/<marker>` (directory bundle), then
  `<module_dir>/<marker>` (flat side-by-side). The marker is the plugin's
  meta json (`axon_meta.json` / `plugin_meta.json`), so a stray `Resources/`
  dir can't shadow the real layout.
  - **Directory bundle** (what `native/clap/package_linux.sh` stages —
    the canonical install form):
    `Axon.clap/` (plain dir) containing `axon.clap` (the ELF .so, rpath
    `$ORIGIN`), `libonnxruntime.so.<ver>` + SONAME symlink, and
    `Resources/` (meta + sub-bundles + `ui/`).
  - **Flat side-by-side**: a bare `.clap` binary with the same resource
    files next to it — for hosts that only pick up regular `*.clap` files.

**Everything else that landed:** `FetchOnnxRuntime.cmake` grew a SHA-pinned
`linux-x64` prebuilt (same 1.20.1 pin); `CMakeLists.txt` now builds ALL
targets on Linux via two platform variables (`AXON_SHIM_SRC` adds
`accelerate_shim.cpp` to every vDSP consumer, `AXON_DSP_LIBS` swaps
`-framework Accelerate` ↔ `axon_pffft`+libm); `axon_clap` links a headless
GUI stub (`axon_gui_stub.cpp`) on non-Apple; `axon_bench`'s bundle-binary
lookup understands all three layouts; ubuntu-latest CI job builds, packages
the dir-bundle and runs the full ctest suite.

**The per-platform oracle (acceptance caveat above, now closed):**
`tests/test_tolerance_stages.cpp` runs MelLimiter, SpectralMaskEq,
IirFilterbankEq, BassMono, Widener, Reverb and LoudnessMeter on
deterministic signals (sines at band centers, xorshift pink noise — no
`std::*_distribution`, which is implementation-defined) and asserts
magnitude/level invariants with documented tolerances (±0.1–0.75 dB point
checks, exact for contractual passthroughs, invariant bands for the
nonlinear stages). It runs on macOS too — verified green against BOTH
backends (real vDSP, and pffft via the sed-`__APPLE__` probe trick).

**Deliberately NOT ported in Phase 1:** the GUI (stub only; CLAP generic
param UI is the interface), `install --linux` CLI sugar, and any
cross-platform render comparison (impossible per the acceptance caveat).

### Phase 2 (2026-07-06, Windows x64 headless) — SHIPPED

**Toolchain: clang-cl + Ninja on `windows-latest`** (`-DCMAKE_C_COMPILER=
clang-cl -DCMAKE_CXX_COMPILER=clang-cl`, MSVC dev env via
`ilammy/msvc-dev-cmd`). Why clang-cl and not `cl`: the DSP/timing headers use
clang/gcc `__builtin_*` intrinsics (`__builtin_clzll` in
`axon_stage_timing.h`), and one compiler front-end across all three platforms
keeps conformance/warning behavior aligned with the reference mac build —
while still linking the MSVC runtime the ORT prebuilt DLL expects. The WIN32
CMake branch adds the global defines `_USE_MATH_DEFINES` (UCRT's `M_PI`),
`NOMINMAX` and `WIN32_LEAN_AND_MEAN`.

**What a Windows .clap is:** the plugin DLL renamed to `.clap`
(`AXON_PLUGIN_SUFFIX=.dll` at build; `package_windows.py` stages the same
directory-bundle layout as Linux — `Axon.clap/` containing `axon.clap`,
`onnxruntime.dll` (+ `onnxruntime_providers_shared.dll`), and `Resources/`).
`resource_path.hpp`'s `GetModuleFileNameW` branch and the same
`Resources/<marker>` → flat probe rule cover discovery; `CLAP_EXPORT` already
expands to `__declspec(dllexport)` for `clap_entry`, nothing extra needed.

**The DLL-search story (no rpath on Windows):** hosts must load plugins with
`LoadLibraryExW(abs_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)` so the
plugin's own dependents (`onnxruntime.dll`) resolve from the plugin's
directory, not the host `.exe`'s. `axon_bench` does exactly this via the new
`src/dyn_module.hpp` (dlopen/LoadLibrary seam; POSIX branch is verbatim the
old dlopen flags). Test executables get the DLLs they need copied next to
them post-build (`onnxruntime.dll` for `test_ort_session`, `sndfile.dll` for
`axon_bench`).

**Windows prebuilts, SHA-pinned:** ORT `win-x64` 1.20.1 zip
(`78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581`; import
lib `lib/onnxruntime.lib`, DLL `lib/onnxruntime.dll`) in
`FetchOnnxRuntime.cmake`; libsndfile 1.2.2 win64 zip
(`2173935c0c1ed13cf627951d34483f9d405ead2eb473190461c42ba220643a3f`) in the
new `FetchSndfileWin.cmake` (bench-only — the plugins never link sndfile).
The `ORTCHAR_T` wide-path helper (`ort_path.hpp`, Phase 0) is now actually
exercised at both `Ort::Session` sites by Windows CI.

**Tests:** all 28 ctest targets run on Windows CI, none skipped —
`test_ssl_integration` runs against a packaged dir-bundle (exercising
LoadLibraryEx + `GetModuleFileNameW` discovery + wide-path ORT bring-up end
to end), and `test_accelerate_shim`'s cross-backend sections are
self-comparisons off-mac by construction (global vDSP names ARE the portable
ones there). New in this phase: `test_resource_path`, which unit-tests BOTH
per-platform resource-layout rules on every platform (they're plain inline
functions now), `module_path_from_addr` against the real per-OS API, and the
`ort_model_path` boundary (POSIX identity-by-reference; UTF-8→UTF-16 on
Windows). Portability fixes: mkstemp/unistd temp files in
`test_meta`/`test_ort_session` got `_WIN32` branches; python-based tests use
a configure-time `Python3_EXECUTABLE` on Windows (`/usr/bin/env python3`
kept verbatim on POSIX).

**Deliberately NOT in Phase 2:** GUI (Phase 3: WebView2 over the same C ABI;
the gui extension still only advertises COCOA so Windows hosts show generic
params), installer/`install --win` sugar, ARM64 Windows, code signing.

### Phase 3 (2026-07-06, GUI backends: webkit2gtk + WebView2) — CODE SHIPPED, RUNTIME UNVERIFIED

**One C ABI, three backends, one page.** `ui/index.html` and the
`axon_gui.h` C ABI are unchanged; the two new backends implement the ABI
one-to-one against the mac reference (`axon_gui.mm`):

- **Linux — `src/axon_gui_gtk.cpp`** (webkit2gtk-4.1, 4.0 fallback):
  `WebKitWebView` inside a `GtkPlug` XEmbed'ed into the host's X11 window
  (`CLAP_WINDOW_API_X11`; `clap_window.x11` XID). The page's existing
  `window.webkit.messageHandlers.axon.postMessage(...)` is webkit2gtk's
  NATIVE script-message API, so the page runs unmodified; payloads are
  serialized with `jsc_value_to_json` and decoded natively. Threading: a
  lazily-spawned fallback thread runs the GLib default main context;
  GLib context OWNERSHIP arbitrates against GTK-based hosts that iterate
  it themselves (closures run on whichever thread owns the context — see
  the file-header comment). `gtk_disable_setlocale()` is called so GTK
  can't flip `LC_NUMERIC` under the `%g` bridge formatting.
- **Windows — `src/axon_gui_win.cpp`** (WebView2, `CLAP_WINDOW_API_WIN32`):
  an owned child HWND is `SetParent`'ed into the host window; the
  environment/controller bring-up is ASYNC (completion callbacks pumped
  by the host's message loop), with pre-ready calls buffered — same idea
  as the mac impl's pre-page-load init buffering. WebView2 has no
  `webkit.messageHandlers`, so `AddScriptToExecuteOnDocumentCreated`
  injects a shim (runs before all page scripts) that forwards
  `postMessage` payloads as `JSON.stringify` strings through
  `chrome.webview.postMessage` into `WebMessageReceived`. The SDK
  (1.0.2903.40) is SHA-pinned in `cmake/FetchWebView2.cmake`; we link
  `WebView2LoaderStatic.lib` so NO new DLL is staged next to the .clap
  (`package_windows.py` unchanged). User-data dir:
  `%LOCALAPPDATA%\Axon\WebView2`.

**Shared bridge core — `src/axon_gui_bridge.hpp`** (header-only, no
platform includes): `build_init_js()` (the `axonInit({...})` handshake,
moved VERBATIM from `axon_gui.mm`, which now calls it too),
`decode_bridge_message()` (dependency-free JSON decoder for the two
page→native shapes, used by both new backends), `file_url_from_path()`
(POSIX/drive/UNC → `file://`), and the WebView2 bootstrap script. All of
it is pinned by `test_axon_gui_bridge` (the suite's 29th ctest target,
run on ALL THREE platform suites: golden axonInit bytes, decoder happy
paths, 20+ malformed-input rejections, file-URL shapes, shim tokens).

**CLAP plumbing:** `axon_plugin.cpp` selects `AXON_GUI_WINDOW_API`
(cocoa/win32/x11) for `is_api_supported`/`get_preferred_api`/`set_parent`
per platform; `AXON_HAS_GUI` (a CMake compile definition) withholds
`CLAP_EXT_GUI` entirely on stub builds so hosts show generic params
instead of embedding a dead view.

**Build gating:** `-DAXON_GUI=OFF` forces the headless stub anywhere.
On Linux the backend auto-enables only if `webkit2gtk-4.1` (or `-4.0`)
+ `x11` resolve via pkg-config — a headless box still builds with zero
new deps. `-DAXON_REQUIRE_GUI=ON` (set by the Linux AND Windows CI jobs)
turns silent fallback into a configure error so CI can't quietly stop
compiling the backends. NOTE: a GUI-enabled Linux .clap now hard-links
webkit2gtk — it will fail to dlopen on hosts without the library
installed; ship `-DAXON_GUI=OFF` builds for such targets.

**HONESTY / runtime acceptance NOT verified.** CI proves the two
backends COMPILE AND LINK (and the bridge logic is unit-tested
everywhere), but no CI can prove a window opens inside a real host.
The following manual scripts are REQUIRED before calling Phase 3 done;
run each on a physical machine:

*Linux (Ubuntu 24.04 + X11 session; host: Bitwig or REAPER):*
 1. `sudo apt install libwebkit2gtk-4.1-dev libx11-dev libsndfile1-dev pkg-config`
 2. `cmake -S native/clap -B native/clap/build -DCMAKE_BUILD_TYPE=Release -DAXON_REQUIRE_GUI=ON && cmake --build native/clap/build -j`
 3. `bash native/clap/package_linux.sh axon "$PWD/weights/axon_bundle" ~/.clap/Axon.clap`
 4. In the host: insert Axon on a stereo track → the embedded UI must
    render `ui/index.html` (dark control surface, not a blank/black box).
 5. Knob → DSP: drag EQ amount; host generic-param view (or automation
    lane) must show the value move. DSP → GUI: automate `EQ` from the
    host; the web knob must follow (axonSetParam path).
 6. Drag a processor card to reorder; verify audio chain order changes
    (e.g. saturator before/after EQ audibly differs at high drive).
 7. Close/reopen the editor 5x; unload the plugin; no host crash, no
    zombie `WebKitWebProcess` after host exit.
 8. Wayland session (no XWayland host window): GUI is expected
    UNAVAILABLE; verify the host falls back to generic params and audio
    still runs.
*Windows (Win10/11 x64; host: REAPER or Bitwig):*
 1. Build per the CI recipe (VS Build Tools + clang-cl + Ninja), then
    `python native/clap/package_windows.py axon weights/axon_bundle "%LOCALAPPDATA%\Programs\Common\CLAP\Axon.clap"`.
 2. Steps 4–7 as on Linux (window embed via WS_CHILD; watch for focus /
    keyboard capture oddities typical of SetParent embeds).
 3. WebView2-runtime-missing case: on a machine/VM WITHOUT the Evergreen
    WebView2 runtime (rare on Win11, common on server SKUs), the plugin
    must degrade to generic params (gui_create returns false; stderr
    logs the HRESULT) — no crash. Decide whether to bundle a runtime
    bootstrapper in the installer story (still open).
*macOS (regression):* open the editor, verify UI + knob round-trip
    unchanged (the mm now routes through the shared init builder).

**Known runtime risks to watch during manual testing** (all flagged in
the backend file headers): GTK-in-plugin-process symbol/loop clashes in
GTK-based hosts; `XInitThreads` ordering if the host used Xlib first;
WebView2 `ExecuteScript` before controller-ready being dropped (by
design — the init handshake re-fires on `gui_show`); HiDPI scaling on
both platforms (`gui_set_scale` currently returns false).

**Deliberately NOT in Phase 3:** Wayland embedding, HiDPI scale
plumbing, a WebView2 runtime bootstrapper/installer, `install --linux/
--win` CLI sugar, code signing.
