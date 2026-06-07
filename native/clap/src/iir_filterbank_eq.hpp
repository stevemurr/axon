// Minimum-phase IIR filterbank EQ — a drop-in alternative *renderer* for the
// neural Auto-EQ. Takes the SAME per-band sigmoid targets as SpectralMaskEq
// (controller unchanged) but realizes the curve with a cascade of N peaking
// (bell) biquads — one per mel band — instead of an STFT magnitude mask.
//
// Why: no STFT framing → no transient smearing, no musical noise, ~0 latency,
// no min-phase-cepstrum ill-conditioning, lower CPU. The peaking biquads are
// themselves minimum-phase.
//
// Faithful match: overlapping bells interact (their dB responses add in the
// cascade), so setting each bell's gain to the target-at-its-center overshoots.
// We precompute the small-signal interaction matrix A (A[i][j] = dB response of
// a unit-gain bell j at center i) and its inverse once; at runtime we solve
//   bell_gains = A⁻¹ · target_curve   (24×24 mat-vec per block)
// so the SUMMED response hits the per-band targets. Gains are time-smoothed.
//
// Header-only, pure DSP (Accelerate not required) — unit-testable standalone.
// process() is mono per instance (the plugin runs one per channel), matching
// SpectralMaskEq's contract: reset(cfg) / set_params(bands,n) / process(in,out,n).

#pragma once
#include <array>
#include <cmath>
#include <vector>

#include "meta.hpp"   // SpectralMaskEqParams (reused for drop-in compatibility)

namespace nablafx {

class IirFilterbankEq {
public:
    void reset(const SpectralMaskEqParams& cfg) {
        cfg_   = cfg;
        sr_    = cfg.sample_rate > 0 ? (double)cfg.sample_rate : 44100.0;
        nb_    = cfg.n_bands;
        min_db_ = cfg.min_gain_db;
        max_db_ = cfg.max_gain_db;

        // Mel band centers (HTK), matching SpectralMaskEq::build_mel_.
        const double mmin = 2595.0 * std::log10(1.0 + cfg.f_min / 700.0);
        const double mmax = 2595.0 * std::log10(1.0 + cfg.f_max / 700.0);
        center_.assign(nb_, 0.0);
        for (int b = 0; b < nb_; ++b) {
            const double mel = mmin + (mmax - mmin) * (b + 1) / (nb_ + 1);
            center_[b] = 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
            center_[b] = std::min(center_[b], 0.49 * sr_);
        }
        // Per-band Q from neighbour spacing (bell spans roughly to its neighbours).
        q_.assign(nb_, 1.0);
        for (int b = 0; b < nb_; ++b) {
            const double fl = (b > 0)      ? center_[b - 1] : center_[b] / std::pow(center_[1] / center_[0], 1.0);
            const double fr = (b < nb_ - 1) ? center_[b + 1] : center_[b] * std::pow(center_[nb_ - 1] / center_[nb_ - 2], 1.0);
            const double bw = std::max(1.0, 0.5 * (fr - fl));
            q_[b] = std::max(0.5, std::min(8.0, center_[b] / bw));
        }

        bells_.assign(nb_, Biquad{});
        gain_db_.assign(nb_, 0.0);
        target_db_.assign(nb_, 0.0);
        smoothed_db_.assign(nb_, 0.0);

        build_interaction_matrix_();   // A and A⁻¹ (depends only on centers/Q)

        // Smoother: ~25 ms time constant, stepped once per set_params (block).
        const double blocks_per_tau = (sr_ * 0.025) / std::max(1, cfg.block_size);
        alpha_ = std::exp(-1.0 / std::max(1.0, blocks_per_tau));

        range_norm_ = 1.0f;
        recompute_coeffs_();
    }

    void set_range_norm(float r) { range_norm_ = r < 0.f ? 0.f : (r > 1.f ? 1.f : r); }

    // Same control contract as SpectralMaskEq: n sigmoid values in [0,1].
    void set_params(const float* bands, std::size_t n) {
        const int m = std::min((int)n, nb_);
        const double span = max_db_ - min_db_;
        for (int b = 0; b < m; ++b) {
            double g = bands[b]; g = g < 0 ? 0 : (g > 1 ? 1 : g);
            target_db_[b] = (min_db_ + g * span) * range_norm_;
        }
        // Time-smooth the target curve, then solve for bell gains so the summed
        // response matches it, then refresh coefficients.
        for (int b = 0; b < nb_; ++b)
            smoothed_db_[b] = alpha_ * smoothed_db_[b] + (1.0 - alpha_) * target_db_[b];
        for (int i = 0; i < nb_; ++i) {
            double s = 0.0;
            for (int j = 0; j < nb_; ++j) s += ainv_[i * nb_ + j] * smoothed_db_[j];
            gain_db_[i] = s;
        }
        recompute_coeffs_();
    }

    // In-place safe. Mono. Zero latency.
    void process(const float* in, float* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            double x = (double)in[i];
            for (int b = 0; b < nb_; ++b) x = bells_[b].process(x);
            out[i] = (float)x;
        }
    }

    int latency_samples() const { return 0; }

    // Sample the realized magnitude (dB) at an arbitrary frequency — for UI /
    // tests. Product of all bell responses.
    double magnitude_db(double f) const {
        const double w = 2.0 * M_PI * f / sr_;
        double db = 0.0;
        for (int b = 0; b < nb_; ++b) db += bells_[b].mag_db(w);
        return db;
    }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        double process(double x) {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void set(double nb0, double nb1, double nb2, double na1, double na2) {
            b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
        }
        // |H(e^jw)| in dB without disturbing state.
        double mag_db(double w) const {
            const double cw = std::cos(w), c2 = std::cos(2 * w), sw = std::sin(w), s2 = std::sin(2 * w);
            const double nr = b0 + b1 * cw + b2 * c2, ni = -(b1 * sw + b2 * s2);
            const double dr = 1.0 + a1 * cw + a2 * c2, di = -(a1 * sw + a2 * s2);
            const double num = nr * nr + ni * ni, den = dr * dr + di * di;
            return 10.0 * std::log10(std::max(1e-30, num / den));
        }
    };

    // RBJ peaking-EQ coefficients for a given center, Q, gain(dB).
    static void peaking(double fc, double q, double gain_db, double sr, Biquad& bq) {
        const double A  = std::pow(10.0, gain_db / 40.0);
        const double w0 = 2.0 * M_PI * fc / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * std::max(1e-6, q));
        const double a0 = 1.0 + alpha / A;
        bq.set((1.0 + alpha * A) / a0, (-2.0 * cw) / a0, (1.0 - alpha * A) / a0,
               (-2.0 * cw) / a0, (1.0 - alpha / A) / a0);
    }

    // RBJ shelving (slope S=1). low=false → high shelf. Edge bands use shelves
    // so a tilt/shelf target is held below/above the outermost centers (peaking
    // bells roll off there and undershoot — the dominant magnitude-match error).
    static void shelf(double fc, double gain_db, double sr, bool low, Biquad& bq) {
        const double A  = std::pow(10.0, gain_db / 40.0);
        const double w0 = 2.0 * M_PI * fc / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / 2.0 * std::sqrt(2.0);     // S = 1
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        double b0, b1, b2, a0, a1, a2;
        if (low) {
            b0 =      A * ((A + 1) - (A - 1) * cw + tsa);
            b1 =  2 * A * ((A - 1) - (A + 1) * cw);
            b2 =      A * ((A + 1) - (A - 1) * cw - tsa);
            a0 =          (A + 1) + (A - 1) * cw + tsa;
            a1 =     -2 * ((A - 1) + (A + 1) * cw);
            a2 =          (A + 1) + (A - 1) * cw - tsa;
        } else {
            b0 =      A * ((A + 1) + (A - 1) * cw + tsa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 =      A * ((A + 1) + (A - 1) * cw - tsa);
            a0 =          (A + 1) - (A - 1) * cw + tsa;
            a1 =      2 * ((A - 1) - (A + 1) * cw);
            a2 =          (A + 1) - (A - 1) * cw - tsa;
        }
        bq.set(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
    }

    // Dispatch by band index: lowest = low shelf, highest = high shelf, else bell.
    void filter_for_(int b, double gain_db, Biquad& bq) const {
        if (b == 0)            shelf(center_[b], gain_db, sr_, /*low*/true, bq);
        else if (b == nb_ - 1) shelf(center_[b], gain_db, sr_, /*low*/false, bq);
        else                   peaking(center_[b], q_[b], gain_db, sr_, bq);
    }

    void recompute_coeffs_() {
        for (int b = 0; b < nb_; ++b) filter_for_(b, gain_db_[b], bells_[b]);
    }

    // A[i][j] = dB response at center i of bell j set to +1 dB. Invert once.
    void build_interaction_matrix_() {
        std::vector<double> A((size_t)nb_ * nb_, 0.0);
        Biquad probe;
        for (int j = 0; j < nb_; ++j) {
            filter_for_(j, 1.0, probe);
            for (int i = 0; i < nb_; ++i) {
                const double w = 2.0 * M_PI * center_[i] / sr_;
                A[(size_t)i * nb_ + j] = probe.mag_db(w);
            }
        }
        ainv_.assign((size_t)nb_ * nb_, 0.0);
        invert_(A, ainv_, nb_);
    }

    // Gauss-Jordan inverse (small N; runs once at reset, not on the audio thread).
    static void invert_(std::vector<double> a, std::vector<double>& inv, int n) {
        for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) inv[(size_t)i * n + j] = (i == j) ? 1.0 : 0.0;
        for (int col = 0; col < n; ++col) {
            int piv = col; double best = std::fabs(a[(size_t)col * n + col]);
            for (int r = col + 1; r < n; ++r) { double v = std::fabs(a[(size_t)r * n + col]); if (v > best) { best = v; piv = r; } }
            if (piv != col) for (int j = 0; j < n; ++j) {
                std::swap(a[(size_t)col * n + j], a[(size_t)piv * n + j]);
                std::swap(inv[(size_t)col * n + j], inv[(size_t)piv * n + j]);
            }
            double d = a[(size_t)col * n + col];
            if (std::fabs(d) < 1e-12) d = (d < 0 ? -1e-12 : 1e-12);
            const double invd = 1.0 / d;
            for (int j = 0; j < n; ++j) { a[(size_t)col * n + j] *= invd; inv[(size_t)col * n + j] *= invd; }
            for (int r = 0; r < n; ++r) {
                if (r == col) continue;
                const double f = a[(size_t)r * n + col];
                if (f == 0.0) continue;
                for (int j = 0; j < n; ++j) {
                    a[(size_t)r * n + j]   -= f * a[(size_t)col * n + j];
                    inv[(size_t)r * n + j] -= f * inv[(size_t)col * n + j];
                }
            }
        }
    }

    SpectralMaskEqParams cfg_{};
    double sr_ = 44100.0;
    int    nb_ = 0;
    float  min_db_ = -12.f, max_db_ = 12.f;
    float  range_norm_ = 1.f;
    double alpha_ = 0.0;

    std::vector<double> center_, q_;
    std::vector<double> gain_db_, target_db_, smoothed_db_;
    std::vector<double> ainv_;     // [nb*nb]
    std::vector<Biquad> bells_;
};

}  // namespace nablafx
