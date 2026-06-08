// Aphex-style aural exciter: adds bright, program-related harmonic "sheen" to
// an upper band and blends a *small* amount on top of the untouched dry signal,
// so the overall tonal balance barely moves. This is the classic band-limited
// parallel excitation topology (Aphex Aural Exciter, 1975):
//
//   input ──┬───────────────────────────────────────────────► (+) ── out
//           │                                                    ▲
//           └─► band-split (HPF, opt LPF) ─► [oversample → even- │
//                  biased low-order waveshaper → downsample] ─► amount ┘
//
// Why each piece (see docs/research_harmonic_excitation.md):
//   * Band-limited (§3 rule 3, §4): only the upper band is distorted, so the
//     full-range dry path is never coloured. An HPF picks the band; an optional
//     LPF ("Tame") bounds the top so the manufactured harmonics don't get fizzy.
//   * Parallel small blend (§4, §6): only a touch of the excited band is summed
//     onto the dry signal — tonal shift is bounded by the Amount control.
//   * Even-biased, low-order harmonics (§2, §3 rules 1-2): the waveshaper favours
//     the 2nd harmonic (octave — warm/consonant) with a controllable touch of
//     3rd (the "grit"). EVEN harmonics come from an ASYMMETRIC curve; ODD from a
//     SYMMETRIC one (tanh). A "Character" knob crossfades warmth↔grit.
//   * Amplitude-proportional (§3 rule 4): the shaper is a soft tanh-type curve,
//     so quiet input → little excitation, loud input → more — it "breathes".
//   * Anti-aliasing (§5.3, the #1 thing that separates silky from harsh): the
//     nonlinearity runs OVERSAMPLED (4×) behind a windowed-sinc anti-image /
//     anti-alias FIR, then is decimated — deliberately working in the top
//     octaves means aliasing would fold straight into the audible band.
//   * DC-free: the asymmetric (even) curve introduces a DC term; we measure it
//     at the current operating point and subtract it, exactly like the Saturator.
//
// Header-only, pure DSP in namespace nablafx, NO CLAP/ORT/std deps beyond the
// standard library — unit-testable standalone (cf. bass_mono.hpp, rational_a.hpp).
//
//   Exciter ex;
//   ex.prepare(44100.0);
//   ex.set_params(/*amount*/0.3f, /*freq*/3000.f, /*drive_db*/6.f,
//                 /*character*/0.25f, /*tame_hz*/18000.f);
//   ex.process(L, R, n);     // in place, stereo
//
// Latency: the up/down FIR pair is linear-phase. The group delay is reported by
// latency_samples() (in base-rate samples) so the host can compensate it; the
// IIR band-split is ~0 latency.

#pragma once
#include <array>
#include <cmath>

namespace nablafx {

class Exciter {
public:
    // 4× polyphase windowed-sinc oversampler. 32 taps gives a clean transition
    // and a small, fixed footprint (same technique as TruePeakCeiling). The
    // up and down FIRs share these coefficients (both band-limit to the
    // original Nyquist), so the total group delay is one FIR length.
    static constexpr int kOvs     = 4;
    static constexpr int kFirTaps = 128;
    static constexpr int kFirPh   = kFirTaps / kOvs;   // taps per polyphase phase
    // The up + down FIRs each add (kFirTaps-1)/2 oversampled samples of group
    // delay, so the wet (excited) band trails its input by (kFirTaps-1) /kOvs
    // base-rate samples (= 31.75 at 128 taps / 4×). The wet still contains the
    // band's fundamental, so summing it against the UNDELAYED dry combs the
    // overlap band. We delay the dry by the nearest integer to that group delay
    // before the sum, so dry and wet are time-aligned (residual < 0.5 sample →
    // any leftover notch sits above Nyquist and is inaudible).
    static constexpr int kDryDelay = (kFirTaps - 1 + kOvs / 2) / kOvs;  // round(31.75)=32

    // The three ring buffers (up history kFirPh, down history kFirTaps, dry
    // delay kDryDelay) are all powers of two, so their wrap can use a cheap
    // bitmask (& (N-1)) instead of a signed integer modulo in the hot inner
    // loops. Static-assert that invariant so a future taps/ovs change can't
    // silently make the mask wrong.
    static_assert((kFirPh   & (kFirPh   - 1)) == 0, "kFirPh must be a power of two");
    static_assert((kFirTaps & (kFirTaps - 1)) == 0, "kFirTaps must be a power of two");
    static_assert((kDryDelay & (kDryDelay - 1)) == 0, "kDryDelay must be a power of two");

    void prepare(double sample_rate) {
        sr_ = sample_rate > 0.0 ? sample_rate : 44100.0;
        build_fir_();
        reset();      // fresh channel state FIRST — reset() default-constructs
        design_();    // each Channel, so coefficients must be designed AFTER it
    }

    // Harmonic-generator selection. BiasedTanh = the original Aphex-style soft curve
    // (infinite harmonic series, band-limited by 4x oversampling). Polynomial =
    // a Chebyshev-class monomial shaper (u^2 -> pure 2nd, u^3 -> pure 3rd) with
    // NO content above the 3rd harmonic, so it barely aliases at 4x and gives
    // exactly-controllable 2nd/3rd. Default is BiasedTanh (unchanged behavior).
    enum class Shaper { BiasedTanh, Polynomial };
    void set_shaper(Shaper s) { shaper_ = s; }

    void reset() {
        for (auto& c : ch_) c = Channel{};
        // reset() default-constructs each Channel, which leaves the band-split
        // biquads at passthrough (Biquad default b0=1). Invalidate the cached
        // cutoffs so the NEXT set_params() re-designs the filters. Without this,
        // CLAP's reset()-after-activate sequence wipes the coefficients designed
        // in prepare(), and because the host's first param push matches the
        // cached defaults, set_params() skips the re-design — leaving the filters
        // passthrough so the high-drive shaper distorts the FULL-RANGE signal
        // (broadband fizz) until a knob is moved. -1 = stale (cf. Saturator).
        hpf_fc_ = -1.f;
        lpf_fc_ = -1.f;
    }

    // amount    : 0..1   parallel wet/dry blend of the excited band (0 = bypass)
    // freq_hz   : band HPF cutoff (the excited band starts here)
    // drive_db  : input gain into the shaper (how hard the band is excited)
    // character : 0 = pure even (2nd, warm) .. 1 = more 3rd (grit)
    // tame_hz   : band LPF cutoff (>= ~19 kHz disables it)
    void set_params(float amount, float freq_hz, float drive_db,
                    float character, float tame_hz) {
        amount_    = amount < 0.f ? 0.f : (amount > 1.f ? 1.f : amount);
        character_ = character < 0.f ? 0.f : (character > 1.f ? 1.f : character);
        drive_     = std::pow(10.f, drive_db / 20.f);
        if (freq_hz != hpf_fc_) { hpf_fc_ = freq_hz; design_hpf_(); }
        use_lpf_ = tame_hz < 19000.f;
        if (use_lpf_ && tame_hz != lpf_fc_) { lpf_fc_ = tame_hz; design_lpf_(); }
        // Operating-point DC: the asymmetric (even) curve maps 0 → nonzero.
        // Subtract that constant so silence stays silence and the output is AC.
        dc_ = shape_(0.0);
    }

    // In place, stereo. When amount == 0 the input is bit-identical to bypass.
    //
    // wet_mono_out (optional): when non-null, receives the per-sample WET
    // CONTRIBUTION that is summed onto the dry — i.e. exactly `amount_ * wet_ac`
    // (post wet-HPF, post-amount), as a mono value. This is observational only:
    // the audio output (buf) is identical whether or not it is supplied. We use
    // the LEFT channel's wet (the stereo image of the exciter is matched, and a
    // mono tap is all the spectrum display needs). It is filled for ALL n samples
    // even on the amount==0 early-out (then it's all zeros, which is correct: the
    // exciter adds nothing). Default nullptr keeps existing callers bit-identical.
    void process(float* l, float* r, int n, float* wet_mono_out = nullptr) {
        if (amount_ <= 0.f) {
            if (wet_mono_out) {
                for (int i = 0; i < n; ++i) wet_mono_out[i] = 0.f;
            }
            return;
        }
        process_ch_(ch_[0], l, n, wet_mono_out);
        if (r) process_ch_(ch_[1], r, n, nullptr);
    }

    // Group delay the plugin should add to its reported latency, in base-rate
    // samples. The up FIR delays by (kFirTaps-1)/2 oversampled samples and the
    // down FIR by the same; the total is one full FIR length at the oversampled
    // rate, i.e. (kFirTaps-1)/kOvs base-rate samples. Rounded to an integer —
    // the residual fractional delay is a fraction of a base sample and inaudible.
    int latency_samples() const {
        // The stage's main (dry) path is delayed by kDryDelay to time-align it
        // with the wet, so that — not the truncated FIR delay — is the latency
        // the host must compensate.
        return kDryDelay;   // 32 samples (= round((kFirTaps-1)/kOvs) at 128/4x)
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
        void clear() { z1 = z2 = 0; }
        void set(double nb0, double nb1, double nb2, double na1, double na2) {
            b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
        }
    };

    struct Channel {
        Biquad hpf{}, lpf{};
        // High-pass on the shaped (wet) signal at the band's lower edge (the
        // same cutoff as the band-split HPF). The EVEN (asymmetric/rectifying)
        // part of the shaper demodulates the high-frequency band's amplitude
        // ENVELOPE down to low frequencies, and intermodulation between band
        // partials produces difference tones — both pile up below the band as
        // sub-bass/DC junk that grows with drive. Everything we WANT (the band
        // fundamental + its 2nd/3rd harmonics) sits at or above the band edge,
        // so high-passing the wet there removes the artifact without touching
        // the excitation. This also subsumes DC blocking.
        Biquad wet_hpf{};
        // Up FIR input history (most-recent-first ring of base-rate samples).
        std::array<double, kFirPh> up_hist{};
        int   up_idx = 0;
        // Down FIR input history at the oversampled rate.
        std::array<double, kFirTaps> dn_hist{};
        int   dn_idx = 0;
        // Dry delay line: aligns the untouched dry with the wet's FIR group
        // delay so the parallel sum doesn't comb (see kDryDelay).
        std::array<double, kDryDelay> dry_ring{};
        int   dry_idx = 0;
    };

    // -- Even/odd waveshaper -------------------------------------------------
    // Both paths come from a single biased tanh, split into its purely-even and
    // purely-odd parts so each knob position targets one harmonic family with no
    // leakage. For g(u) = tanh(u + bias):
    //   even part  e(x) = ½(g(x) + g(-x))   — an EVEN function → 2nd-dominant
    //                                          (the asymmetry/bias makes the 2nd)
    //   odd  part  o(x) = ½(g(x) − g(-x))   — an ODD function  → 3rd-dominant grit
    // The even part is purely even by construction, so it has NO odd leakage even
    // at high drive — keeping Character=0 cleanly 2nd-dominant. Soft (tanh) keeps
    // it low-order and amplitude-proportional (quiet in → little excitation).
    // Character crossfades: 0 = pure even (warm), 1 = pure odd (grit).
    double shape_(double x) const {
        if (shaper_ == Shaper::Polynomial) {
            // Chebyshev-class monomial shaper. u^2 is a pure 2nd-harmonic
            // generator (cos²θ = ½+½cos2θ → DC, removed by the wet-HPF, + 2nd),
            // u^3 a pure 3rd (cos³θ = ¾cosθ+¼cos3θ → fundamental + 3rd). Neither
            // produces ANY harmonic above the 3rd, so at 4× oversampling the
            // shaped band cannot alias (3·Nyquist < oversampled Nyquist), and
            // Character cleanly trades 2nd↔3rd with no higher-order leakage.
            // Clamp keeps the polynomials bounded if driven past unity (only the
            // extreme regime; at musical drive |u|≲1 so it never engages).
            double u = drive_ * x;
            if (u >  1.0) u =  1.0;
            else if (u < -1.0) u = -1.0;
            return (1.0 - character_) * (u * u) + character_ * (u * u * u);
        }
        const double xe = drive_ * x;
        const double gp = std::tanh( xe + kEvenBias);
        const double gm = std::tanh(-xe + kEvenBias);
        const double even = 0.5 * (gp + gm);   // even → 2nd (+DC, removed by dc_)
        const double odd  = 0.5 * (gp - gm);   // odd  → 3rd
        return (1.0 - character_) * even + character_ * odd;
    }

    void process_ch_(Channel& c, float* buf, int n, float* wet_mono_out = nullptr) {
        for (int i = 0; i < n; ++i) {
            const double x = static_cast<double>(buf[i]);

            // 1) Band-split: HPF (then optional LPF) extracts the excite band.
            double band = c.hpf.process(x);
            if (use_lpf_) band = c.lpf.process(band);

            // 2) Oversample → shape → decimate. Push the band sample into the
            //    up FIR history, evaluate the kOvs polyphase outputs, shape each
            //    at the oversampled rate, then run them through the decimation
            //    FIR. The down FIR's output for this base sample is the value
            //    aligned with the just-pushed input (anti-alias lowpass).
            c.up_hist[c.up_idx] = band;
            c.up_idx = (c.up_idx + 1) & (kFirPh - 1);

            double wet = 0.0;
            for (int p = 0; p < kOvs; ++p) {
                // Up-sampled sample for polyphase phase p (the polyphase FIR
                // already includes the kOvs gain via its DC normalization).
                double up = 0.0;
                for (int k = 0; k < kFirPh; ++k) {
                    const int tap   = p + k * kOvs;
                    const int h_idx = (c.up_idx + kFirPh - 1 - k) & (kFirPh - 1);
                    up += fir_[tap] * c.up_hist[h_idx];
                }
                // Nonlinearity at the oversampled rate.
                const double sh = shape_(up) - dc_;

                // Feed into the decimation FIR history.
                c.dn_hist[c.dn_idx] = sh;
                c.dn_idx = (c.dn_idx + 1) & (kFirTaps - 1);

                // Only the last sub-phase produces an output sample (decimate
                // by kOvs). Convolve the down FIR over the oversampled history.
                if (p == kOvs - 1) {
                    double acc = 0.0;
                    for (int k = 0; k < kFirTaps; ++k) {
                        const int h_idx = (c.dn_idx + kFirTaps - 1 - k) & (kFirTaps - 1);
                        acc += fir_[k] * c.dn_hist[h_idx];
                    }
                    // The down FIR also carries the kOvs DC gain (it sums kOvs
                    // contributions per output); divide it back out.
                    wet = acc / static_cast<double>(kOvs);
                }
            }

            // 3) High-pass the wet at the band edge to strip the sub-band junk
            //    the even/rectifying shaper manufactures (demodulated HF envelope
            //    + intermodulation difference tones + DC), then parallel-blend:
            //    add a small amount of the excited band on top of the dry — but
            //    the dry must be delayed to match the wet's FIR group delay
            //    (kDryDelay), otherwise the in-band fundamental carried by the
            //    wet combs against the dry.
            const double wet_ac = c.wet_hpf.process(wet);
            const double dry_d = c.dry_ring[c.dry_idx];   // x delayed kDryDelay samples
            c.dry_ring[c.dry_idx] = x;
            c.dry_idx = (c.dry_idx + 1) & (kDryDelay - 1);
            const double added = amount_ * wet_ac;        // the contribution summed on
            buf[i] = static_cast<float>(dry_d + added);
            // Observational wet tap: exactly what we add onto the dry. Read-only
            // — the line above is computed identically with or without the tap.
            if (wet_mono_out) wet_mono_out[i] = static_cast<float>(added);
        }
    }

    // -- Coefficient design --------------------------------------------------
    void design_() { design_hpf_(); design_lpf_(); }

    // 2nd-order Butterworth high-pass (RBJ), Q = 1/√2. Defines the excite band.
    void design_hpf_() {
        const double fc = (hpf_fc_ > 1.0 && hpf_fc_ < 0.49 * sr_) ? hpf_fc_ : 3000.0;
        const double w0 = 2.0 * M_PI * fc / sr_;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double Q  = 0.70710678;
        const double alpha = sw / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        const double hb0 = (1.0 + cw) / 2.0 / a0, hb1 = -(1.0 + cw) / a0;
        const double ha1 = -2.0 * cw / a0, ha2 = (1.0 - alpha) / a0;
        for (auto& c : ch_) {
            c.hpf.set(hb0, hb1, hb0, ha1, ha2);
            // Same cutoff on the wet post-shaper HPF: the wet must add nothing
            // below the band (see Channel::wet_hpf).
            c.wet_hpf.set(hb0, hb1, hb0, ha1, ha2);
        }
    }

    // 2nd-order Butterworth low-pass ("Tame") to keep the top from fizzing.
    void design_lpf_() {
        const double fc = (lpf_fc_ > 1.0 && lpf_fc_ < 0.49 * sr_) ? lpf_fc_ : 18000.0;
        const double w0 = 2.0 * M_PI * fc / sr_;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double Q  = 0.70710678;
        const double alpha = sw / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        for (auto& c : ch_)
            c.lpf.set((1.0 - cw) / 2.0 / a0, (1.0 - cw) / a0,
                      (1.0 - cw) / 2.0 / a0, -2.0 * cw / a0, (1.0 - alpha) / a0);
    }

    // 4× zero-stuffed reconstruction / decimation lowpass via windowed sinc.
    // Cutoff = original Nyquist = Fs_in/2, Hann window, normalized so the DC
    // gain across the kOvs polyphase phases is kOvs (so per-phase upsample gain
    // is unity; the decimation pass divides the kOvs sum back out).
    void build_fir_() {
        constexpr int N = kFirTaps;
        const double fc_norm = 0.5 / static_cast<double>(kOvs);   // 0.125 at 4×
        const double center  = 0.5 * (N - 1);
        double sum = 0.0;
        for (int n = 0; n < N; ++n) {
            const double k = static_cast<double>(n) - center;
            const double s = (std::abs(k) < 1e-9)
                ? 2.0 * fc_norm
                : std::sin(2.0 * M_PI * fc_norm * k) / (M_PI * k);
            const double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (N - 1)));
            fir_[n] = s * w;
            sum += fir_[n];
        }
        const double scale = static_cast<double>(kOvs) / sum;
        for (double& v : fir_) v *= scale;
    }

    static constexpr double kEvenBias = 0.5;   // asymmetry → 2nd-harmonic bias

    double sr_      = 44100.0;
    float  amount_  = 0.f;
    float  character_ = 0.25f;
    double drive_   = 1.0;
    float  hpf_fc_  = 3000.f;
    float  lpf_fc_  = 18000.f;
    bool   use_lpf_ = true;
    double dc_      = 0.0;
    Shaper shaper_  = Shaper::BiasedTanh;   // default = original behavior

    std::array<double, kFirTaps> fir_{};
    std::array<Channel, 2>       ch_{};
};

}  // namespace nablafx
