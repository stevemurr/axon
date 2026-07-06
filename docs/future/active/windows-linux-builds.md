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
