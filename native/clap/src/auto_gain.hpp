// Auto gain (level-matched bypass / "gain match").
//
// Drives the processed OUTPUT loudness toward the INPUT loudness so that
// engaging the chain is loudness-neutral — you A/B timbre & dynamics honestly
// instead of being fooled by "louder = better".
//
// Feed-forward on short-term LUFS: out_lufs is the REAL (uncompensated) output
// loudness, so the target offset (in − out) is independent of the gain we apply
// — a simple one-pole smoother toward it, no feedback loop. Call once per block
// and multiply the *delivered* output by the returned gain AFTER metering the
// real output, so the OUT meter still shows the true master loudness.
//
//   AutoGain ag;
//   float g = ag.process(enabled, meter_in.lufs_s, meter_out.lufs_s); // real out
//   out *= g;   // monitoring trim, after the meter tap

#pragma once
#include <algorithm>
#include <cmath>

namespace nablafx {

class AutoGain {
public:
    void reset() { g_db_ = 0.f; last_ff_db_ = 0.f; }

    // enabled  — whether auto gain is on
    // in_lufs  — input short-term LUFS
    // out_lufs — short-term LUFS of the REAL (uncompensated) output
    // ff_db    — a *known* output gain (e.g. limiter Drive × wet) fed forward so
    //            the monitoring level doesn't jump while the slow LUFS loop
    //            catches up; only its per-call *delta* is injected.
    // Returns the linear gain to apply this block.
    float process(bool enabled, float in_lufs, float out_lufs, float ff_db) {
        if (!enabled) {
            // Smoothly relax back to unity when disabled; keep tracking ff_db so
            // re-enabling doesn't inject the gain that changed while we were off.
            g_db_ *= kRelease;
            if (std::fabs(g_db_) < 0.01f) g_db_ = 0.f;
            last_ff_db_ = ff_db;
            return std::pow(10.f, g_db_ / 20.f);
        }

        // Feed-forward: cancel the known gain change instantly. The LUFS loop
        // below then trims the (small, nonlinear) residual; the injection only
        // shapes the transient since the loop always converges to `target`.
        g_db_ += -(ff_db - last_ff_db_);
        last_ff_db_ = ff_db;

        // Only adapt when real signal is present, so silence (LUFS at the floor)
        // doesn't wind the gain up.
        if (in_lufs > kFloor && out_lufs > kFloor) {
            const float target = std::clamp(in_lufs - out_lufs, -kMaxDb, kMaxDb);
            g_db_ += (target - g_db_) * kSmooth;        // one-pole toward target
        }
        g_db_ = std::clamp(g_db_, -kMaxDb, kMaxDb);
        return std::pow(10.f, g_db_ / 20.f);
    }

    float gain_db() const { return g_db_; }

private:
    static constexpr float kFloor   = -50.f;   // LUFS gate
    static constexpr float kMaxDb   = 24.f;    // clamp
    static constexpr float kSmooth  = 0.004f;  // per-block one-pole toward target
    static constexpr float kRelease = 0.995f;  // per-block decay toward 0 when off

    float g_db_      = 0.f;
    float last_ff_db_ = 0.f;
};

}  // namespace nablafx
