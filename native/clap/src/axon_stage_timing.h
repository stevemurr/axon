// axon_stage_timing.h — per-stage CPU timing instrumentation (bench-only).
//
// Shared POD ABI between the plugin (writer) and the bench harness (reader).
// The plugin exposes it as a custom CLAP extension (AXON_EXT_STAGE_TIMING)
// ONLY when compiled with -DAXON_STAGE_TIMING=1 (cmake option; see
// CMakeLists.txt / build.sh). Default builds never define the macro, never
// touch this header from the audio path, and return nullptr for the
// extension id — the instrumentation is compile-time zero-cost when off.
//
// THREADING CONTRACT (deliberate — do not "fix" by adding atomics):
//   * The audio thread is the only writer (plain, non-atomic stores).
//   * Readers (count/get/reset) may only be called while process() is NOT
//     running — i.e. by a bench/offline host between processing runs, or
//     after the last process() call and BEFORE deactivate() (deactivate
//     clears plug->chains; the bank itself survives, but stats after
//     deactivate describe a dead instance).
//   * No atomics / fences by design: they would add cost to the audio hot
//     path and perturb the very numbers this exists to measure. This is
//     bench-only tooling, not a shipping surface.
//
// Clock: clock_gettime_nsec_np(CLOCK_UPTIME_RAW) on macOS — monotonic,
// ~20-40 ns per read, no kernel trap on Apple Silicon. Other platforms fall
// back to std::chrono::steady_clock (also monotonic; a little more per-read
// overhead, which is fine for this bench-only tooling).
//
// Histogram: log2 buckets — hist[b] counts samples with dt_ns in
// [2^(b-1), 2^b) (b = min(31, 64 - clz(dt|1))). Percentiles reconstructed
// from the histogram are approximate (within 2x, linearly interpolated
// inside a bucket); max_ns is exact so the worst case is always covered.

#pragma once

#include <stdint.h>
#include <string.h>

#ifdef __APPLE__
#include <time.h>       // clock_gettime_nsec_np
#else
#include <chrono>       // std::chrono::steady_clock fallback
#endif

#include <clap/clap.h>

// Extension id the plugin answers to in get_extension() (instrumented builds
// only; default builds return nullptr for it).
#define AXON_EXT_STAGE_TIMING "axon.stage-timing/1"

// One timing slot. POD — copied wholesale across the plugin/bench boundary.
struct axon_stage_timing_entry {
    char     name[24];     // NUL-terminated slot name (fixed layout, see below)
    uint64_t calls;        // number of recorded intervals
    uint64_t total_ns;     // sum of interval durations
    uint64_t max_ns;       // exact worst-case interval
    uint32_t hist[32];     // log2 duration histogram (see header comment)
    uint8_t  is_subtimer;  // 1 = nested inside a parent stage's time
};

// Custom CLAP extension vtable. Query with:
//   plug->get_extension(plug, AXON_EXT_STAGE_TIMING)
// Returns nullptr on non-instrumented builds.
struct axon_stage_timing {
    uint32_t (*count)(const clap_plugin_t* plugin);
    bool     (*get)(const clap_plugin_t* plugin, uint32_t index,
                    axon_stage_timing_entry* out);
    void     (*reset)(const clap_plugin_t* plugin);
};

// ---------------------------------------------------------------------------
// Fixed slot layout. Slots 1-9 mirror the plugin's StageID enum values
// (slot 0 unused, like the enum); then pseudo-stages (plumbing timed around
// the stage loop); then sub-timers (nested inside their parent stage's time).
// The names below are a contract — downstream tooling keys on them.
// ---------------------------------------------------------------------------
enum {
    AXON_ST_UNUSED          = 0,   // mirrors the unused StageID 0
    AXON_ST_AUTO_EQ         = 1,   // StageID::AutoEQ
    AXON_ST_RETIRED_2       = 2,   // retired StageID 2 (was Saturator; removed 2026-07)
    AXON_ST_SSL_EQ          = 3,   // StageID::SslEq
    AXON_ST_SSL_COMP        = 4,   // StageID::SslComp
    AXON_ST_MEL_LIMITER     = 5,   // StageID::MelLimiter
    AXON_ST_BASS_MONO       = 6,   // StageID::BassMono
    AXON_ST_RETIRED_7       = 7,   // retired StageID 7 (was Exciter; removed 2026-07)
    AXON_ST_REVERB          = 8,   // StageID::Reverb
    AXON_ST_WIDENER         = 9,   // StageID::Widener
    // Pseudo-stages (fixed plumbing inside flush_chain_block_):
    AXON_ST_METER_IN        = 10,
    AXON_ST_SPECTRUM_PUSH   = 11,
    AXON_ST_TRIM_CEILING    = 12,
    AXON_ST_METER_OUT       = 13,
    AXON_ST_AUTO_GAIN       = 14,
    // Sub-timers (is_subtimer = 1; nested inside their parent stage time):
    AXON_ST_SSL_ORT_FORWARD = 15,  // inside SslComp: ssl_comp_ort->run()
    AXON_ST_AUTOEQ_ORT_CTRL = 16,  // inside AutoEQ: LSTM run_controller()+swap
    AXON_ST_SLOT_COUNT      = 17,
};

static const char* const axon_stage_timing_slot_names[AXON_ST_SLOT_COUNT] = {
    "Unused",
    "AutoEQ", "Retired2", "SslEq", "SslComp", "MelLimiter",
    "BassMono", "Retired7", "Reverb", "Widener",
    "MeterIn", "SpectrumPush", "TrimCeiling", "MeterOut", "AutoGain",
    "SslOrtForward", "AutoEqOrtCtrl",
};

// Nanosecond monotonic clock.
#ifdef __APPLE__
// macOS: no kernel trap on Apple Silicon.
static inline uint64_t axon_st_now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}
#else
// Portable fallback: steady_clock is monotonic on every platform we target
// (Linux: CLOCK_MONOTONIC; Windows: QueryPerformanceCounter).
static inline uint64_t axon_st_now(void) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
#endif

// Writer-side accumulator bank (lives inside the plugin instance; the
// extension vtable serves entries out of it). Audio thread only — see the
// threading contract at the top of this header.
struct AxonStageTimingBank {
    axon_stage_timing_entry entries[AXON_ST_SLOT_COUNT];

    void reset() {
        memset(entries, 0, sizeof(entries));
        for (int i = 0; i < AXON_ST_SLOT_COUNT; ++i) {
            // Names fit in name[24] by construction (longest is 13 chars).
            strncpy(entries[i].name, axon_stage_timing_slot_names[i],
                    sizeof(entries[i].name) - 1);
        }
        entries[AXON_ST_SSL_ORT_FORWARD].is_subtimer = 1;
        entries[AXON_ST_AUTOEQ_ORT_CTRL].is_subtimer = 1;
    }

    // Record one interval of dt_ns into slot. Hot path: two adds, one
    // compare, one clz-derived histogram bump. No atomics (see contract).
    // Portability note: __builtin_clzll is available on clang, gcc AND
    // clang-cl (the port's Windows toolchain). Only an MSVC-frontend build
    // would need a _BitScanReverse64 alternative — not a supported toolchain.
    void record(int slot, uint64_t dt_ns) {
        axon_stage_timing_entry& e = entries[slot];
        e.calls    += 1;
        e.total_ns += dt_ns;
        if (dt_ns > e.max_ns) e.max_ns = dt_ns;
        int b = 64 - __builtin_clzll(dt_ns | 1ull);  // 1..64
        if (b > 31) b = 31;
        e.hist[b] += 1;
    }
};
