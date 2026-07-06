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
