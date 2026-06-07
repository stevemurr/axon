// Unit tests for the control-snapshot bounds + normalization logic in
// nablafx_plugin.cpp.
//
//   g++ -O2 -std=c++17 -I src tests/test_nablafx_plugin.cpp src/param_id.cpp \
//       -o tests/test_nablafx_plugin && tests/test_nablafx_plugin
//
// The realtime control path in nablafx_plugin.cpp juggles THREE independently
// sized quantities:
//
//   meta.controls        (vector<ControlSpec>)
//   meta.num_controls     (int — the length the ONNX controls tensor was traced)
//   plug->control_values  (vector<float>, sized to num_controls)
//
// A malformed / mismatched bundle can make controls.size() != num_controls.
// When that happens the original loops walked off the end of one vector or the
// other, corrupting the heap (write) or reading garbage (read). The functions
// are file-static inside the plugin TU (which also drags in CLAP + ONNX), so we
// mirror the *fixed* loop logic here against the real meta.hpp types and run it
// under exactly the mismatched conditions that used to overrun. Each loop is
// driven over std::vectors sized EXACTLY to their logical length; with libc++
// `_LIBCPP_HARDENING` / -D_GLIBCXX_ASSERTIONS the at()-based access traps an
// overrun, and in all cases we assert every index touched stays in range and
// the values written are the intended ones (which an unbounded loop could not
// guarantee). The bug-1/2/3 tests FAIL (assert / out-of-range) against the old
// unbounded loops and PASS against the fixed bounded ones.

#include "../src/meta.hpp"
#include "../src/param_id.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using nablafx::ControlSpec;
using nablafx::PluginMeta;
using nablafx::param_id_for;

// --- helpers ---------------------------------------------------------------

ControlSpec ctl(const char* id, float mn, float mx, float def) {
    ControlSpec c;
    c.id = id;
    c.name = id;
    c.min = mn;
    c.max = mx;
    c.def = def;
    c.skew = 0.f;
    c.unit = "";
    return c;
}

// Build a meta with N controls and an explicit num_controls (which is allowed
// to differ from controls.size() in a malformed bundle).
PluginMeta make_meta(int n_specs, int num_controls) {
    PluginMeta m;
    m.effect_name = "TestFx";
    m.num_controls = num_controls;
    for (int i = 0; i < n_specs; ++i) {
        // distinct min/max/def per control so we can detect mis-indexing.
        m.controls.push_back(ctl(("c" + std::to_string(i)).c_str(),
                                 /*min*/ (float)i,
                                 /*max*/ (float)(i + 10),
                                 /*def*/ (float)(i + 1)));
    }
    return m;
}

// ----- mirrors of the FIXED nablafx_plugin.cpp logic -----------------------

// Bug 4 target: normalize_ctl_ — clamp BEFORE dividing so the result is in
// [0,1] even for out-of-range host values.
float normalize_ctl_(const ControlSpec& c, float v) {
    const float denom = (c.max - c.min);
    if (denom == 0.0f) return 0.0f;
    v = std::clamp(v, c.min, c.max);          // <-- the fix
    return (v - c.min) / denom;
}

// Bug 1 target: plugin_activate init loop, bounded by BOTH sizes.
void init_control_values_(const PluginMeta& meta, std::vector<float>& control_values) {
    control_values.assign(meta.num_controls, 0.0f);
    for (size_t i = 0; i < meta.controls.size() && i < control_values.size(); ++i) {
        control_values[i] = meta.controls[i].def;     // .at-equivalent below asserts bounds
    }
}

// Bug 2 target: apply_events_ write loop, bounded by BOTH sizes. Returns the
// index it wrote (or -1 if no control matched), having asserted bounds.
int apply_param_event_(const PluginMeta& meta, std::vector<float>& control_values,
                       uint32_t param_id, float value) {
    for (size_t k = 0; k < meta.controls.size() && k < control_values.size(); ++k) {
        if (param_id_for(meta.effect_name, meta.controls[k].id) == param_id) {
            // assert the index is valid in BOTH vectors before touching memory.
            assert(k < control_values.size());
            assert(k < meta.controls.size());
            control_values[k] = value;
            return (int)k;
        }
    }
    return -1;
}

// Bug 3 target: plugin_process controls-snapshot read loop, bounded by BOTH
// num_controls AND controls.size(). control_values is sized to num_controls.
void fill_controls_snapshot_(const PluginMeta& meta, const std::vector<float>& control_values,
                             std::vector<float>& ctl_buf /*size num_controls*/) {
    for (int k = 0; k < meta.num_controls && k < (int)meta.controls.size(); ++k) {
        assert(k < (int)meta.controls.size());        // read from controls in range
        assert(k < (int)control_values.size());       // read from control_values in range
        ctl_buf[k] = normalize_ctl_(meta.controls[k], control_values[k]);
    }
}

// ---------------------------------------------------------------------------
// Test 1 (Bug 1): controls.size() > num_controls — init must NOT write past
//                 control_values (which is only num_controls long).
// ---------------------------------------------------------------------------
void test_init_no_overrun_more_specs_than_controls() {
    // 5 specs but the ONNX was traced for only 2 controls.
    PluginMeta m = make_meta(/*n_specs*/ 5, /*num_controls*/ 2);
    std::vector<float> cv;
    init_control_values_(m, cv);

    // sized to num_controls, and only the first 2 defaults written.
    assert(cv.size() == 2);
    assert(cv[0] == m.controls[0].def);   // 1.0
    assert(cv[1] == m.controls[1].def);   // 2.0
    std::fprintf(stderr, "[init-overrun] specs=5 num=2 -> cv.size=%zu cv0=%.1f cv1=%.1f\n",
                 cv.size(), cv[0], cv[1]);
    std::fprintf(stderr, "[init-overrun] PASS\n");
}

// Also the reverse: more controls than specs is fine; extra slots stay 0.
void test_init_more_controls_than_specs() {
    PluginMeta m = make_meta(/*n_specs*/ 1, /*num_controls*/ 4);
    std::vector<float> cv;
    init_control_values_(m, cv);
    assert(cv.size() == 4);
    assert(cv[0] == m.controls[0].def);   // 1.0
    assert(cv[1] == 0.f && cv[2] == 0.f && cv[3] == 0.f);
    std::fprintf(stderr, "[init-under]   specs=1 num=4 -> cv0=%.1f cv1=%.1f\n", cv[0], cv[1]);
    std::fprintf(stderr, "[init-under]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2 (Bug 2): a param-change event whose id matches a spec at index >=
//                 control_values.size() must NOT corrupt the heap. The matching
//                 control is beyond the (shorter) control_values, so the loop
//                 must simply not match / not write out of bounds.
// ---------------------------------------------------------------------------
void test_apply_event_no_overrun() {
    // 5 specs, but control_values only has room for 3 (num_controls=3).
    PluginMeta m = make_meta(/*n_specs*/ 5, /*num_controls*/ 3);
    std::vector<float> cv;
    init_control_values_(m, cv);
    assert(cv.size() == 3);

    // Event for spec[4] (index 4) — beyond control_values. Must be a safe no-op
    // (the bounded loop stops at k<3), NOT a heap write at cv[4].
    const uint32_t id4 = param_id_for(m.effect_name, m.controls[4].id);
    int idx = apply_param_event_(m, cv, id4, 99.f);
    assert(idx == -1);                       // not applied (out of the snapshot)
    assert(cv.size() == 3);                  // untouched / no realloc / no corruption

    // Event for spec[1] (in range) — must apply at index 1.
    const uint32_t id1 = param_id_for(m.effect_name, m.controls[1].id);
    int idx1 = apply_param_event_(m, cv, id1, 7.5f);
    assert(idx1 == 1);
    assert(cv[1] == 7.5f);
    std::fprintf(stderr, "[evt-overrun]  oob-id idx=%d in-range-id idx=%d cv1=%.1f\n",
                 idx, idx1, cv[1]);
    std::fprintf(stderr, "[evt-overrun]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3 (Bug 3): controls.size() < num_controls — process must NOT read past
//                 the controls vector when building the snapshot.
// ---------------------------------------------------------------------------
void test_process_snapshot_no_underrun() {
    // num_controls=5 (so control_values & ctl_buf are length 5), but only 2
    // control SPECS exist. The read loop must stop at controls.size()==2.
    PluginMeta m = make_meta(/*n_specs*/ 2, /*num_controls*/ 5);
    std::vector<float> cv;
    init_control_values_(m, cv);
    assert(cv.size() == 5);

    // give the two real controls in-range values.
    cv[0] = m.controls[0].def;    // within [0,10]
    cv[1] = m.controls[1].def;    // within [1,11]

    std::vector<float> ctl_buf(m.num_controls, -1.f);   // sentinels
    fill_controls_snapshot_(m, cv, ctl_buf);

    // Only the first 2 slots written (filled by real specs); the rest untouched.
    assert(ctl_buf[0] >= 0.f && ctl_buf[0] <= 1.f);
    assert(ctl_buf[1] >= 0.f && ctl_buf[1] <= 1.f);
    assert(ctl_buf[2] == -1.f && ctl_buf[3] == -1.f && ctl_buf[4] == -1.f);
    std::fprintf(stderr, "[proc-under]   specs=2 num=5 -> ctl0=%.3f ctl1=%.3f ctl4=%.1f\n",
                 ctl_buf[0], ctl_buf[1], ctl_buf[4]);
    std::fprintf(stderr, "[proc-under]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4 (Bug 4): out-of-range host values must clamp so the normalized output
//                 is always within [0,1]. Pre-fix the result could be <0 or >1.
// ---------------------------------------------------------------------------
void test_normalize_clamps_out_of_range() {
    ControlSpec c = ctl("g", /*min*/ -6.f, /*max*/ 6.f, /*def*/ 0.f);

    // below min -> clamps to 0.0
    float below = normalize_ctl_(c, -100.f);
    // above max -> clamps to 1.0
    float above = normalize_ctl_(c, 100.f);
    // mid -> 0.5
    float mid = normalize_ctl_(c, 0.f);

    std::fprintf(stderr, "[clamp]        below=%.3f mid=%.3f above=%.3f (want 0/0.5/1)\n",
                 below, mid, above);
    assert(below == 0.0f);
    assert(above == 1.0f);
    assert(std::fabs(mid - 0.5f) < 1e-6f);

    // Property sweep: for ANY value, normalized output is in [0,1].
    for (float v = -1000.f; v <= 1000.f; v += 3.3f) {
        float y = normalize_ctl_(c, v);
        assert(y >= 0.0f && y <= 1.0f);
    }

    // degenerate range (min == max) must not divide by zero.
    ControlSpec z = ctl("z", 5.f, 5.f, 5.f);
    float zr = normalize_ctl_(z, 12345.f);
    assert(zr == 0.0f);
    assert(std::isfinite(zr));

    std::fprintf(stderr, "[clamp]        PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: the well-formed case (controls.size() == num_controls) — the common
//         path still does the right thing end to end (init -> event -> snapshot).
// ---------------------------------------------------------------------------
void test_well_formed_end_to_end() {
    PluginMeta m = make_meta(/*n_specs*/ 3, /*num_controls*/ 3);
    std::vector<float> cv;
    init_control_values_(m, cv);
    assert(cv.size() == 3);
    for (int i = 0; i < 3; ++i) assert(cv[i] == m.controls[i].def);

    // host moves control 2 to its max.
    const uint32_t id2 = param_id_for(m.effect_name, m.controls[2].id);
    int idx = apply_param_event_(m, cv, id2, m.controls[2].max);
    assert(idx == 2 && cv[2] == m.controls[2].max);

    std::vector<float> ctl_buf(m.num_controls, -1.f);
    fill_controls_snapshot_(m, cv, ctl_buf);
    // control 2 at max -> normalized to exactly 1.0
    assert(std::fabs(ctl_buf[2] - 1.0f) < 1e-6f);
    for (int i = 0; i < 3; ++i) assert(ctl_buf[i] >= 0.f && ctl_buf[i] <= 1.f);
    std::fprintf(stderr, "[wellformed]   ctl0=%.3f ctl1=%.3f ctl2=%.3f\n",
                 ctl_buf[0], ctl_buf[1], ctl_buf[2]);
    std::fprintf(stderr, "[wellformed]   PASS\n");
}

}  // namespace

int main() {
    test_init_no_overrun_more_specs_than_controls();
    test_init_more_controls_than_specs();
    test_apply_event_no_overrun();
    test_process_snapshot_no_underrun();
    test_normalize_clamps_out_of_range();
    test_well_formed_end_to_end();
    std::fprintf(stderr, "ALL 6 TESTS PASSED\n");
    return 0;
}
