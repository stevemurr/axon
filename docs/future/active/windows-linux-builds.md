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
