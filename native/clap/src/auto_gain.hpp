// Auto gain (level-matched bypass / "gain match").
//
// Drives the processed OUTPUT loudness toward the INPUT loudness so that
// engaging the chain is loudness-neutral — you A/B timbre & dynamics honestly
// instead of being fooled by "louder = better".
//
// It's a slow feedback integrator on short-term LUFS: because the output meter
// reflects the *compensated* output, the loop self-corrects until out ≈ in and
// the error vanishes. Call once per processing block; multiply the output by the
// returned linear gain (applied pre-ceiling, so the common attenuating case
// can't break the true-peak guarantee).
//
//   AutoGain ag;
//   float g = ag.process(enabled, meter_in.lufs_s, meter_out.lufs_s);
//   out *= g;   // before the final ceiling

#pragma once
#include <algorithm>
#include <cmath>

namespace nablafx {

class AutoGain {
public:
    void reset() { g_db_ = 0.f; }

    // enabled  — whether auto gain is on
    // in_lufs  — input short-term LUFS
    // out_lufs — short-term LUFS of the (already compensated) output
    // Returns the linear gain to apply this block.
    float process(bool enabled, float in_lufs, float out_lufs) {
        if (enabled) {
            // Only integrate when real signal is present, so silence (LUFS at
            // the floor) doesn't wind the gain up.
            if (in_lufs > kFloor && out_lufs > kFloor) {
                const float err = in_lufs - out_lufs;       // want out → in
                g_db_ = std::clamp(g_db_ + err * kIntegrate, -kMaxDb, kMaxDb);
            }
        } else {
            // Smoothly relax back to unity when disabled.
            g_db_ *= kRelease;
            if (std::fabs(g_db_) < 0.01f) g_db_ = 0.f;
        }
        return std::pow(10.f, g_db_ / 20.f);
    }

    float gain_db() const { return g_db_; }

private:
    static constexpr float kFloor     = -50.f;   // LUFS gate
    static constexpr float kMaxDb     = 24.f;    // clamp
    static constexpr float kIntegrate = 0.002f;  // per-block loop coefficient
    static constexpr float kRelease   = 0.995f;  // per-block decay toward 0 when off

    float g_db_ = 0.f;
};

}  // namespace nablafx
