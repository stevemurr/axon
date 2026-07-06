// Standalone unit tests for the grey-box DSP runtime blocks.
//
// Build (Linux/macOS):
//   g++ -O2 -std=c++17 -I../src test_grey_dsp.cpp ../src/meta.cpp \
//       -I<path-to-nlohmann-json/include> -o test_grey_dsp && ./test_grey_dsp
//
// Both DSP classes are header-only and have no CLAP/ORT dependencies, so this
// runs anywhere the meta loader can.

#include "../src/meta.hpp"
#include "../src/rational_a.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Coefficients are stored as float (matches the JSON payload), so eval()
// matches the reference to float precision (~1e-7), not double precision.
constexpr double kFloatTol = 1e-6;

void test_rational_a_identity() {
    // P(x) = x, Q(x) = 1 → y = x.
    nablafx::RationalA r;
    r.reset(/*numerator=*/{0.0f, 1.0f}, /*denominator=*/{});
    for (double x : {-1.5, -0.3, 0.0, 0.3, 1.5}) {
        double y = r.eval(x);
        assert(std::fabs(y - x) < kFloatTol);
    }
    std::fprintf(stderr, "[rational_a/identity] PASS\n");
}

void test_rational_a_constant() {
    // P(x) = 0.7, Q(x) = 1 → y = 0.7 for all x (numerator length 1 = constant).
    nablafx::RationalA r;
    r.reset({0.7f}, {});
    for (double x : {-2.0, 0.0, 2.0}) {
        double y = r.eval(x);
        assert(std::fabs(y - 0.7) < kFloatTol);
    }
    std::fprintf(stderr, "[rational_a/constant] PASS\n");
}

void test_rational_a_quadratic_over_linear() {
    // P(x) = x + x^2, Q(x) = 1 + |2x| → analytic form, a few hand-computed checks.
    nablafx::RationalA r;
    r.reset({0.0f, 1.0f, 1.0f}, {2.0f});
    auto ref = [](double x) {
        double p = x + x * x;
        double q = 1.0 + std::fabs(2.0 * x);
        return p / q;
    };
    for (double x : {-1.5, -0.7, -0.1, 0.0, 0.1, 0.7, 1.5}) {
        double got = r.eval(x);
        double exp = ref(x);
        assert(std::fabs(got - exp) < kFloatTol);
    }
    std::fprintf(stderr, "[rational_a/quad_over_lin] PASS\n");
}

void test_rational_a_buffer_processing() {
    nablafx::RationalA r;
    r.reset({0.1f, 1.0f, 0.0f, -0.2f}, {0.5f, 0.3f});
    std::vector<float> in(64), out(64);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<float>(std::sin(0.1 * i));
    }
    r.process(in.data(), out.data(), in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        double y = r.eval(static_cast<double>(in[i]));
        assert(std::fabs(static_cast<double>(out[i]) - y) < 1e-6);
    }
    std::fprintf(stderr, "[rational_a/buffer] PASS\n");
}

// Q(x) = 1 + sum_j |b_j * x^j| takes the magnitude PER TERM, not of the whole
// polynomial. With negative coefficients and negative x the two readings give
// wildly different answers (the wrong one can even hit Q == 0), so exact
// known-answer checks here pin the per-term semantics.
//
// All inputs are chosen so every intermediate (Horner steps, |b_j x^j| terms,
// the running sum) is exactly representable, and the expected value is the
// same single IEEE division the implementation performs — so the asserts are
// exact, no tolerance.
void test_rational_a_denominator_per_term_abs() {
    nablafx::RationalA r;
    // P(x) = x, Q(x) = 1 + |-2 x| + |-3 x^2|.
    r.reset({0.0f, 1.0f}, {-2.0f, -3.0f});

    // x = -1: Q = 1 + 2 + 3 = 6 (a non-abs sum would give 1 + 2 - 3 = 0 and
    // divide by zero; abs-of-the-whole-sum would give 1 + |2 - 3| = 2).
    assert(r.eval(-1.0) == -1.0 / 6.0);
    // x = +2: Q = 1 + 4 + 12 = 17.
    assert(r.eval(2.0) == 2.0 / 17.0);

    // Mixed-sign cubic denominator, all terms land on exact powers of two:
    // x = -2: Q = 1 + |0.5*-2| + |-0.25*4| + |0.125*-8| = 1 + 1 + 1 + 1 = 4.
    r.reset({0.0f, 1.0f}, {0.5f, -0.25f, 0.125f});
    assert(r.eval(-2.0) == -0.5);
    std::fprintf(stderr, "[rational_a/den_per_term_abs] PASS\n");
}

// The canonical soft clipper y = x / (1 + |x|): exact known answers at points
// where the quotient is exactly representable, plus the two invariants that
// make it a safe saturator: passivity (|y| <= |x|, since Q >= 1) and a bounded
// output (|y| < 1).
void test_rational_a_soft_clip_invariants() {
    nablafx::RationalA r;
    r.reset({0.0f, 1.0f}, {1.0f});

    // Exact points: 1/2, 3/4, 7/8 are dyadic.
    assert(r.eval(1.0) == 0.5);
    assert(r.eval(-1.0) == -0.5);
    assert(r.eval(3.0) == 0.75);
    assert(r.eval(-7.0) == -0.875);
    assert(r.eval(0.0) == 0.0);

    // Passivity + bound over a buffer. Q(x) = 1 + |x| >= 1 exactly, so the
    // true quotient has |y| <= |x| and |y| < 1; round-to-nearest cannot cross
    // either representable bound for |x| <= 1e6 (headroom 1/(1+|x|) >= ~1e-6
    // dwarfs half an ulp at 1.0 in both double and float).
    std::vector<float> in, out;
    for (int i = -400; i <= 400; ++i) {
        in.push_back(static_cast<float>(i) * 0.05f);       // -20 .. 20
    }
    in.push_back(1e6f);
    in.push_back(-1e6f);
    out.resize(in.size());
    r.process(in.data(), out.data(), in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        assert(std::fabs(out[i]) <= std::fabs(in[i]));     // passivity
        assert(std::fabs(out[i]) < 1.0f);                  // bounded
        assert(std::isfinite(out[i]));
    }
    std::fprintf(stderr, "[rational_a/soft_clip_invariants] PASS\n");
}

// With an odd numerator (even-degree coeffs all zero) the rational is an odd
// function, and the Horner/abs evaluation order preserves that exactly in
// IEEE arithmetic: eval(-x) == -eval(x) bitwise, for any denominator.
void test_rational_a_odd_symmetry() {
    nablafx::RationalA r;
    r.reset({0.0f, 0.9f, 0.0f, -0.35f, 0.0f, 0.02f},
            {0.7f, -0.4f, 0.15f});
    for (double x : {1e-6, 0.013, 0.1, 0.5, 1.0, 1.7, 3.0, 12.5, 300.0}) {
        double yp = r.eval(x);
        double yn = r.eval(-x);
        assert(yn == -yp);  // exact, not a tolerance
        assert(yp != 0.0);  // guard: the sweep actually exercises P and Q
    }
    std::fprintf(stderr, "[rational_a/odd_symmetry] PASS\n");
}

// reset() must REPLACE the coefficient state, and the empty/degenerate
// configurations must be well-defined: empty numerator => y == 0 exactly
// (0 / Q), fully empty => y == 0 (0 / 1) and empty() reports true.
void test_rational_a_reset_and_empty() {
    nablafx::RationalA r;
    assert(r.empty());                       // default-constructed
    assert(r.eval(0.7) == 0.0);              // P = 0, Q = 1

    r.reset({0.0f, 1.0f}, {1.0f});
    assert(!r.empty());
    assert(r.eval(1.0) == 0.5);

    // Re-reset to a pure constant: the old linear/denominator config must not
    // leak through (append instead of replace would change both P and Q).
    r.reset({0.25f}, {});
    assert(!r.empty());
    assert(r.eval(2.0) == 0.25);
    assert(r.eval(-3.0) == 0.25);

    // Empty numerator, non-empty denominator: 0 / Q == 0 for all x.
    r.reset({}, {2.0f});
    assert(!r.empty());                      // denominator alone => not empty
    assert(r.eval(5.0) == 0.0);
    assert(r.eval(-0.1) == 0.0);

    // Fully empty again.
    r.reset({}, {});
    assert(r.empty());
    assert(r.eval(123.0) == 0.0);
    std::fprintf(stderr, "[rational_a/reset_and_empty] PASS\n");
}

// process() buffer-loop edges: n == 0 writes nothing, n == 1 works, the
// buffer path is bitwise-identical to per-sample eval(), it is in-place safe
// as documented, deterministic across runs, and exact for zero/denormal-scale
// inputs (where Q rounds to exactly 1.0 so y == x).
void test_rational_a_process_edges() {
    nablafx::RationalA r;
    r.reset({0.0f, 1.0f}, {1.0f});           // y = x / (1 + |x|)

    // n == 0: out must be untouched.
    {
        float in = 0.5f;
        float out[3] = {123.0f, -456.0f, 789.0f};
        r.process(&in, out, 0);
        assert(out[0] == 123.0f && out[1] == -456.0f && out[2] == 789.0f);
    }

    // n == 1.
    {
        float in = 3.0f, out = 0.0f;
        r.process(&in, &out, 1);
        assert(out == 0.75f);
    }

    // Buffer path == eval path, bitwise; deterministic across runs; in-place
    // (in == out) matches out-of-place bitwise.
    {
        std::vector<float> in = {0.0f,   -0.0f,  1.0f,   -1.0f,  0.3f,
                                 -2.7f,  100.0f, -1e6f,  1e-3f,  -1e-3f,
                                 1e-38f, -1e-38f, 1e-42f, -1e-42f, 7.0f};
        std::vector<float> out1(in.size()), out2(in.size());
        r.process(in.data(), out1.data(), in.size());
        r.process(in.data(), out2.data(), in.size());
        std::vector<float> inplace = in;
        r.process(inplace.data(), inplace.data(), inplace.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            float ref = static_cast<float>(r.eval(static_cast<double>(in[i])));
            assert(out1[i] == ref);          // bitwise vs eval()
            assert(out2[i] == out1[i]);      // deterministic
            assert(inplace[i] == out1[i]);   // in-place safe
        }
        // Zero and denormal-scale inputs: 1 + |x| rounds to exactly 1.0 in
        // double for |x| < 2^-53, so y == x exactly (identity at tiny level —
        // no denormal blow-up or flush-to-nonzero).
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (std::fabs(static_cast<double>(in[i])) < 1e-30) {
                assert(out1[i] == in[i]);
            }
        }
        assert(out1[0] == 0.0f);             // x == +0 stays 0
        assert(out1[1] == 0.0f);             // x == -0 stays 0
    }
    std::fprintf(stderr, "[rational_a/process_edges] PASS\n");
}

// Saturator-shaped config (same degrees as the shipped export: 7 numerator /
// 5 denominator coefficients). Invariant: Q >= 1 exactly, so the denominator
// can only attenuate — |eval(x)| <= |P(x)| where P is replicated here with
// the identical Horner recurrence (same op order in double => bitwise-equal
// reference, no tolerance needed). Outputs stay finite across a wide sweep.
void test_rational_a_saturator_degree_bounds() {
    const std::vector<float> num = {0.01f, 1.2f, -0.05f, -0.8f,
                                    0.02f, 0.15f, -0.003f};
    const std::vector<float> den = {0.9f, 0.5f, 0.3f, 0.1f, 0.05f};
    nablafx::RationalA r;
    r.reset(num, den);

    auto horner = [&](double x) {
        double p = num.back();
        for (std::size_t i = num.size() - 1; i-- > 0;) {
            p = p * x + static_cast<double>(num[i]);
        }
        return p;
    };

    bool denominator_engaged = false;
    for (double x : {-1e6, -1e3, -12.0, -1.0, -0.25, -1e-3, 0.0,
                     1e-3, 0.25, 1.0, 12.0, 1e3, 1e6}) {
        double y = r.eval(x);
        double p = horner(x);
        assert(std::isfinite(y));
        assert(std::fabs(y) <= std::fabs(p));  // Q >= 1 never amplifies
        if (y != p) denominator_engaged = true;
        assert(y == r.eval(x));                // deterministic, bitwise
    }
    assert(denominator_engaged);  // guard: the bound check wasn't vacuous
    std::fprintf(stderr, "[rational_a/saturator_degree_bounds] PASS\n");
}

// Round-trip the schema-v2 bundle metas through load_meta and confirm the
// payload comes out looking right. Driven by an env var so we don't fail on
// machines that don't have the bundles handy.
void test_meta_schema_v2_roundtrip() {
    const char* root = std::getenv("NABLAFX_EXPORTS_ROOT");
    if (!root) {
        std::fprintf(stderr, "[meta/v2] SKIP (set NABLAFX_EXPORTS_ROOT to run)\n");
        return;
    }
    {
        auto m = nablafx::load_meta(std::string(root) + "/saturator/plugin_meta.json");
        assert(m.schema_version == 2);
        assert(m.stage_kind == nablafx::StageKind::Dsp);
        assert(m.architecture == "dsp");
        assert(m.dsp_blocks.size() == 1);
        assert(m.dsp_blocks[0].kind == "rational_a");
        const auto& r = std::get<nablafx::RationalAParams>(m.dsp_blocks[0].params);
        assert(r.numerator.size() == 7);
        assert(r.denominator.size() == 5);
        std::fprintf(stderr, "[meta/v2 saturator] PASS\n");
    }
    {
        auto m = nablafx::load_meta(std::string(root) + "/auto_eq/plugin_meta.json");
        assert(m.schema_version == 2);
        assert(m.stage_kind == nablafx::StageKind::NnDsp);
        assert(m.architecture == "lstm");
        assert(m.dsp_blocks.size() == 1);
        assert(m.dsp_blocks[0].kind == "spectral_mask_eq");
        const auto& sm = std::get<nablafx::SpectralMaskEqParams>(m.dsp_blocks[0].params);
        assert(sm.n_bands > 0);
        assert(sm.n_fft >= sm.hop * 2);
        assert(sm.num_control_params == sm.n_bands);
        assert(sm.block_size == 128);
        std::fprintf(stderr, "[meta/v2 auto_eq] PASS\n");
    }
    {
        auto m = nablafx::load_meta(std::string(root) + "/la2a/plugin_meta.json");
        // LA-2A stayed at v1 originally but now exports as v2 nn-only;
        // accept either.
        assert(m.schema_version == 1 || m.schema_version == 2);
        assert(m.stage_kind == nablafx::StageKind::Nn);
        assert(m.dsp_blocks.empty());
        assert(!m.input_names.empty());
        std::fprintf(stderr, "[meta/v2 la2a] PASS\n");
    }
}

}  // namespace

int main() {
    test_rational_a_identity();
    test_rational_a_constant();
    test_rational_a_quadratic_over_linear();
    test_rational_a_buffer_processing();
    test_rational_a_denominator_per_term_abs();
    test_rational_a_soft_clip_invariants();
    test_rational_a_odd_symmetry();
    test_rational_a_reset_and_empty();
    test_rational_a_process_edges();
    test_rational_a_saturator_degree_bounds();
    test_meta_schema_v2_roundtrip();
    std::fprintf(stderr, "ALL GREY-DSP TESTS PASSED\n");
    return 0;
}
