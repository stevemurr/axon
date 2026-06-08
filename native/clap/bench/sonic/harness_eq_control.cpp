// Adaptive-EQ CONTROLLER A/B harness. Drives each candidate controller over a
// spectrally-distinct material battery and scores:
//   adaptivity   — ‖cross-material per-band curve std‖  (HIGH = responds to
//                  material; a mode-collapsed controller scores ~0)
//   activity     — mean |curve| dB (sanity: a stub emits 0)
//   match        — out-of-tolerance residual MAE vs the tonal-balance target
//   crest_delta  — transient preservation through the full chain (dB)
//   modulation   — musical-noise / zipper on a steady 1 kHz tone (dB)
//   reso_reduce  — resonance peak-to-baseline excess removed (dB, ↑ better)
//   loud_delta   — broadband RMS Δ in→out (dB, ~0 if zero-mean correction works)
//
// Build:
//   c++ -O3 -std=c++17 -I src bench/sonic/harness_eq_control.cpp \
//       -framework Accelerate -o bench/sonic/harness_eq_control
#include "../../src/iir_filterbank_eq.hpp"   // renderer (zero-latency)
#include "analysis.hpp"
#include "signals.hpp"
#include "eq_ctrl.hpp"
#include "eq_ctrl_c1.hpp"
#include "eq_ctrl_c2.hpp"
#include "eq_ctrl_c3.hpp"
#include "eq_ctrl_cascade.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <numeric>
#include <string>

using namespace eqctrl;
using sonic::lin2db;

static EqCtrlCfg CFG() { EqCtrlCfg c; c.sample_rate = 44100; c.block_size = 128; c.n_bands = 24; return c; }

static nablafx::SpectralMaskEqParams rcfg(const EqCtrlCfg& c) {
    nablafx::SpectralMaskEqParams p;
    p.sample_rate = c.sample_rate; p.block_size = c.block_size; p.num_control_params = c.n_bands;
    p.n_fft = 2048; p.hop = 512; p.n_bands = c.n_bands;
    p.min_gain_db = c.min_gain_db; p.max_gain_db = c.max_gain_db;
    p.f_min = c.f_min; p.f_max = c.f_max;
    return p;
}

// ---- material battery (deterministic, spectrally distinct) ------------------
static std::vector<float> brown_noise(int n, uint32_t seed) {
    auto w = sonic::pink_like(n, 1.0, seed);
    std::vector<float> o(n); double acc = 0;
    for (int i = 0; i < n; ++i) { acc = 0.999 * acc + w[i]; o[i] = (float)acc; }
    double pk = 1e-9; for (float v : o) pk = std::max(pk, (double)std::fabs(v));
    for (auto& v : o) v = (float)(0.3 * v / pk);
    return o;
}
static std::vector<float> bright_noise(int n, uint32_t seed) {
    std::mt19937 rng(seed); std::uniform_real_distribution<float> d(-1, 1);
    std::vector<float> o(n); float prev = 0;
    for (int i = 0; i < n; ++i) { float w = d(rng); o[i] = 0.3f * (w - prev); prev = w; }
    return o;
}
// Music-like: chord + noise bed + ticks, with a spectral tilt knob (<0 dark, >0 bright).
static std::vector<float> music_like(int n, double sr, double tilt, uint32_t seed) {
    std::mt19937 rng(seed); std::normal_distribution<float> g(0, 1);
    std::vector<float> o(n, 0.f);
    const double root = 110.0;
    for (int k = 0; k < 5; ++k) {
        const double f = root * (k + 1), a = 0.3 / (k + 1);
        for (int i = 0; i < n; ++i) o[i] += (float)(a * std::sin(2 * sonic::kPi * f * i / sr));
    }
    for (int i = 0; i < n; ++i) o[i] += 0.1f * g(rng);
    // one-pole tilt
    double acc = 0; const double al = std::clamp(std::exp(-std::fabs(tilt) * 0.2), 0.5, 0.99);
    for (int i = 0; i < n; ++i) { acc = al * acc + (1 - al) * o[i]; o[i] = tilt > 0 ? (float)(o[i] - acc) : (float)acc; }
    double pk = 1e-9; for (float v : o) pk = std::max(pk, (double)std::fabs(v));
    for (auto& v : o) v = (float)(0.4 * v / pk);
    return o;
}

struct Sig { std::string name; std::vector<float> x; };
static std::vector<Sig> battery(int n, double sr) {
    return {
        {"brown",       brown_noise(n, 11)},
        {"pink",        sonic::pink_like(n, 0.3, 7)},
        {"white",       [&]{ std::mt19937 r(3); std::uniform_real_distribution<float> d(-.3f,.3f);
                             std::vector<float> o(n); for (auto& v: o) v=d(r); return o; }()},
        {"bright",      bright_noise(n, 5)},
        {"music_dark",  music_like(n, sr, -2.5, 21)},
        {"music_bright",music_like(n, sr, +2.5, 22)},
    };
}

// ---- run a controller over a signal, return its CONVERGED per-band dB curve --
static std::vector<float> converged_curve(IEqController& c, const std::vector<float>& x, const EqCtrlCfg& cfg) {
    c.reset(cfg);
    const int B = cfg.block_size;
    std::vector<float> db(cfg.n_bands, 0.f);
    // target() must be called EVERY block: a controller's emitted-curve time
    // smoother only advances per target() call, so calling it once would report
    // a curve stepped ~1% toward its value. Mirror render_chain()'s cadence.
    for (int i = 0; i + B <= (int)x.size(); i += B) {
        c.observe(&x[i], B);
        c.target(db.data(), cfg.n_bands);
    }
    return db;
}

// ---- run the FULL chain (controller → renderer) streaming, return output -----
static std::vector<float> render_chain(IEqController& c, const std::vector<float>& x, const EqCtrlCfg& cfg) {
    c.reset(cfg);
    nablafx::IirFilterbankEq eq; eq.reset(rcfg(cfg));
    std::vector<float> out(x.size(), 0.f), db(cfg.n_bands), bands(cfg.n_bands);
    const int B = cfg.block_size;
    for (int i = 0; i + B <= (int)x.size(); i += B) {
        c.observe(&x[i], B);
        c.target(db.data(), cfg.n_bands);
        bands_from_db(db.data(), bands.data(), cfg.n_bands, cfg);
        eq.set_params(bands.data(), cfg.n_bands);
        eq.process(&x[i], &out[i], B);
    }
    return out;
}

// long-term input mel-dB spectrum (independent measurement)
static std::vector<float> input_mel_db(const std::vector<float>& x, const EqCtrlCfg& cfg) {
    RunningMelSpectrum s; s.reset(cfg);
    const int B = cfg.block_size;
    for (int i = 0; i + B <= (int)x.size(); i += B) s.observe(&x[i], B);
    std::vector<float> db(cfg.n_bands); s.mel_db(db.data());
    return db;
}

static double rms_db(const std::vector<float>& x) {
    double s = 0; int c = 0; for (size_t i = x.size() / 4; i < x.size(); ++i) { s += (double)x[i] * x[i]; ++c; }
    return lin2db(std::sqrt(s / std::max(1, c)));
}

struct Row { std::string name; double adapt, activity, match, crest_d, modul, reso, loud_d; };

static Row score(const std::function<std::unique_ptr<IEqController>()>& make, const EqCtrlCfg& cfg, double sr) {
    Row r; { auto c = make(); r.name = c->name(); }
    const int n = (int)(sr * 3);
    auto sigs = battery(n, sr);
    auto centers = mel_centers(cfg);

    // --- adaptivity + activity + match over the battery ---
    std::vector<std::vector<float>> curves;
    double activity = 0, match = 0;
    for (auto& s : sigs) {
        auto c = make();
        auto curve = converged_curve(*c, s.x, cfg);
        curves.push_back(curve);
        auto in_db = input_mel_db(s.x, cfg);
        double am = 0; for (float v : curve) am += std::fabs(v); activity += am / cfg.n_bands;
        // SHAPE match: compare the mean-removed output spectrum to the
        // mean-removed target, then credit the per-band tolerance. Level-
        // invariant on purpose — C1's correction is zero-mean, so an absolute
        // offset is not an error; only the tonal SHAPE deviation counts.
        std::vector<double> outd(cfg.n_bands), tgt(cfg.n_bands);
        double om = 0, tm = 0;
        for (int b = 0; b < cfg.n_bands; ++b) {
            outd[b] = in_db[b] + curve[b]; tgt[b] = target_db(centers[b]);
            om += outd[b]; tm += tgt[b];
        }
        om /= cfg.n_bands; tm /= cfg.n_bands;
        double m = 0;
        for (int b = 0; b < cfg.n_bands; ++b) {
            const double resid = (outd[b] - om) - (tgt[b] - tm);
            m += std::max(0.0, std::fabs(resid) - tolerance_db(centers[b]));
        }
        match += m / cfg.n_bands;
    }
    activity /= sigs.size(); match /= sigs.size();
    // per-band std across signals → ‖std‖
    double adapt2 = 0;
    for (int b = 0; b < cfg.n_bands; ++b) {
        double mean = 0; for (auto& cu : curves) mean += cu[b]; mean /= curves.size();
        double var = 0; for (auto& cu : curves) { double d = cu[b] - mean; var += d * d; } var /= curves.size();
        adapt2 += var;
    }
    r.adapt = std::sqrt(adapt2); r.activity = activity; r.match = match;

    // --- transient preservation: click train through the chain ---
    {
        auto c = make(); auto in = sonic::click_train(1 << 15, 2048, 0.7);
        auto out = render_chain(*c, in, cfg);
        r.crest_d = sonic::crest_db(out.data(), (int)out.size(), (int)out.size()/4)
                  - sonic::crest_db(in.data(), (int)in.size(), (int)in.size()/4);
    }
    // --- musical noise: steady 1 kHz through the chain ---
    {
        auto c = make(); auto in = sonic::sine(1000.0, 0.3, n, sr);
        auto out = render_chain(*c, in, cfg);
        r.modul = sonic::modulation_depth_db(out.data(), (int)out.size(), sr, 5.0, n / 3);
    }
    // --- resonance reduction: noise + injected 2.5 kHz resonance ---
    {
        auto c = make();
        auto in = sonic::pink_like(n, 0.25, 31);
        for (int i = 0; i < n; ++i) in[i] += (float)(0.25 * std::sin(2 * sonic::kPi * 2500.0 * i / sr));
        auto out = render_chain(*c, in, cfg);
        auto pre = input_mel_db(in, cfg), post = input_mel_db(out, cfg);
        // band nearest 2.5k
        int rb = 0; double best = 1e9;
        for (int b = 0; b < cfg.n_bands; ++b) { double d = std::fabs(centers[b]-2500.0); if (d<best){best=d;rb=b;} }
        auto excess = [&](const std::vector<float>& m){
            std::vector<float> base(cfg.n_bands); gaussian_smooth_db(m.data(), base.data(), cfg.n_bands, 3.0);
            return m[rb] - base[rb];
        };
        r.reso = excess(pre) - excess(post);   // positive = resonance reduced
    }
    // --- loudness side-effect: RMS delta on pink ---
    {
        auto c = make(); auto in = sonic::pink_like(n, 0.3, 13);
        auto out = render_chain(*c, in, cfg);
        r.loud_d = rms_db(out) - rms_db(in);
    }
    return r;
}

int main() {
    const EqCtrlCfg cfg = CFG(); const double sr = cfg.sample_rate;
    std::printf("EQ-control A/B — SR=%.0f, %d bands, span [%.0f,%.0f] dB\n",
                sr, cfg.n_bands, (double)cfg.min_gain_db, (double)cfg.max_gain_db);
    std::printf("(adapt↑ activity~ match↓ crest_d~0 modul↓ reso↑ loud_d~0)\n\n");

    std::vector<std::function<std::unique_ptr<IEqController>()>> makers = {
        [] { return std::make_unique<C1_TargetMatch>(); },
        [] { return std::make_unique<C2_ResonanceSuppress>(); },
        [] { return std::make_unique<C3_Perceptual>(); },
        [] { return std::make_unique<C1C2_Cascade>(); },
    };

    std::printf("%-22s %7s %8s %7s %8s %7s %6s %7s\n",
                "controller", "adapt", "activity", "match", "crest_d", "modul", "reso", "loud_d");
    for (auto& mk : makers) {
        Row r = score(mk, cfg, sr);
        std::printf("%-22s %7.2f %8.2f %7.2f %8.2f %7.1f %6.2f %7.2f\n",
                    r.name.c_str(), r.adapt, r.activity, r.match, r.crest_d, r.modul, r.reso, r.loud_d);
        std::printf("KV %s adapt=%.3f activity=%.3f match=%.3f crest_d=%.3f modul=%.2f reso=%.3f loud_d=%.3f\n",
                    r.name.c_str(), r.adapt, r.activity, r.match, r.crest_d, r.modul, r.reso, r.loud_d);
    }
    return 0;
}
