// Composite Axon CLAP plugin: 1 dylib that wires
//
//   audio → ort(autoeq controller) → SpectralMaskEq
//         → RationalA (saturator)  → Exciter → ort(ssl_comp) → BassMono
//         → MelLimiter             → TruePeakCeiling → output trim
//
// The composite has two host-exposed knobs (AMT, TRM) defined in the
// composite_meta. AMT remaps to per-stage params (saturator pre/post + wet/dry,
// LA-2A peak reduction, auto-EQ wet/dry); TRM is a final linear gain.
//
// Block-rate streaming: the auto-EQ controller and the LA-2A LSTM both want
// fixed 128-sample blocks (cond_block_size). The plugin accumulates host
// audio into a 128-sample input ring per channel and flushes the chain block
// by block; output samples come out of an output ring with the same depth.
// Total internal latency = 128 (one block of accumulator) + ceiling lookahead.
//
// v1 limitations:
//   - arm64 macOS only (parent CMakeLists guards against other platforms)
//   - CPU execution provider only
//   - per-block parameter snapshot (no sample-accurate smoothing)
//   - refuses activation if host sample rate != composite_meta.sample_rate

#include <algorithm>
#include <atomic>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axon_gui.h"

#include <dlfcn.h>

#include <clap/clap.h>
#include <onnxruntime_cxx_api.h>

#include "composite_meta.hpp"
#include "auto_gain.hpp"
#include "bass_mono.hpp"
#include "coherence_distortion.hpp"
#include "exciter.hpp"
#include "multiband_exciter.hpp"
#include "exciter_spectrum.hpp"
#include "reverb.hpp"
#include "widener.hpp"
#include "meta.hpp"
#include "meter.hpp"
#include "param_id.hpp"
#include "mel_limiter.hpp"
#include "rational_a.hpp"
#include "spectral_mask_eq.hpp"
#include "iir_filterbank_eq.hpp"   // zero-latency renderer (STFT↔IIR toggle)
#include "adaptive_eq.hpp"   // deterministic Auto-EQ controller (Neural↔Adaptive toggle)
#include "ssl_channel_eq.hpp"   // SSL 9000 J channel EQ (analytic biquads; before AutoEQ)
#include "true_peak_ceiling.hpp"

namespace nablafx_axon {

namespace fs = std::filesystem;
using nablafx::CompositeMeta;
using nablafx::ControlSpec;
using nablafx::DspBlockSpec;
using nablafx::PluginMeta;
using nablafx::RationalA;
using nablafx::RationalAParams;
using nablafx::SpectralMaskEq;
using nablafx::SpectralMaskEqParams;
using nablafx::TruePeakCeiling;
using nablafx::load_composite_meta;
using nablafx::load_meta;
using nablafx::param_id_for;

// All ORT sessions in this plugin process audio in fixed kBlockSize chunks,
// which matches both the auto-EQ controller's cond_block_size and the LA-2A
// processor's TVFiLM cond_block_size. Changing this requires re-exporting both
// ONNX bundles at the new block size.
constexpr int kBlockSize  = 128;
// Reorderable stages. The Saturator (StageID 2) AND the Harmonics/Exciter
// (StageID 7) are intentionally NOT in the chain — their enum values, process
// cases, modules (RationalA / MultibandExciter), and params are all KEPT (so
// either can be re-added by putting its id back into the order + bumping this
// count), they're just not wired into processor_order, so they never run and
// have no UI tab. The SSL 9000 J channel EQ (StageID 3) reuses the freed
// OutputLeveler slot and DOES run (before AutoEQ).
constexpr int kNumStages  = 7;
// SSL bus comp accumulator size — must be a multiple of kBlockSize. Larger
// values cut CPU proportionally (1 ORT call per kSslHop samples instead of
// 1 per kBlockSize) at the cost of (kSslHop - kBlockSize) extra latency.
//
// CRITICAL CONSTRAINT: `kSslHop <= trace_len - RF` so every ring shift
// preserves at least RF samples of past context for the model's causal
// convolutions. Asserted at activate.
//
// The wet output is delayed by `kSslHop - kBlockSize` samples relative to
// the dry signal at the blend step. We compensate via a per-channel dry
// delay ring (see ssl_comp_dry_delay) so the wet/dry mix is sample-aligned.
constexpr int kSslHop     = 1024;

// IDs are stable (kept across the leveler removal) so existing automation and
// stage colours don't shuffle; 0 (was InputLeveler) is intentionally unused.
// Slot 3 (was OutputLeveler) now hosts the SSL channel EQ. All remaining stages
// are freely reorderable.
enum class StageID : int {
    AutoEQ        = 1,
    Saturator     = 2,
    SslEq         = 3,   // SSL 9000 J channel EQ (native biquads; sits before AutoEQ)
    SslComp       = 4,
    MelLimiter    = 5,
    BassMono      = 6,
    Exciter       = 7,   // Aphex-style aural exciter (band-limited parallel)
    Reverb        = 8,   // transparent mastering room reverb (8-line FDN)
    Widener       = 9,   // transparent M/S stereo widener (Blumlein shuffler)
};

// ---------------------------------------------------------------------------
// Spectrum analyzer — Goertzel-based, runs on main thread
// ---------------------------------------------------------------------------

struct SpectrumAnalyzer {
    static constexpr int   kFFT     = 2048;   // accumulation window
    static constexpr int   kDisp    = 128;    // log-spaced display bins
    static constexpr int   kNumBins = 50;     // eq_bins resolution (log-spaced 20–20k Hz)
    static constexpr float kAlpha   = 0.65f;  // EMA coefficient (~110 ms at ~21 fps)
    static constexpr float kFlo     = 20.f;
    static constexpr float kFhi   = 20000.f;

    // Audio thread: one mono accumulator per chain position.
    struct Accum {
        std::array<float, kFFT> buf{};
        int fill{0};
    };
    std::array<Accum, kNumStages> accum{};

    // Transfer buffer: audio thread fills, main thread processes.
    std::mutex  xfer_mtx;
    bool        xfer_ready{false};
    std::array<std::array<float, kFFT>, kNumStages> xfer_frames{};
    std::array<float, 5>        xfer_eq_gains_snap{};
    std::array<float, kNumBins> xfer_eq_bins_snap{};
    bool                        xfer_has_bins_snap{false};

    // Main-thread state.
    std::array<float, kFFT>  hann{};
    std::array<float, kDisp> disp_hz{};    // Hz for each display bin
    std::array<float, kFFT>  windowed{};   // scratch for Goertzel input

    // EMA magnitude [chain_pos][disp_bin], linear.
    std::array<std::array<float, kDisp>, kNumStages> ema{};

    // Last EQ band gains (dB) and optional 50-point bin gains from the LSTM.
    // Written from the audio thread, snapped under xfer_mtx, read by main thread.
    std::array<float, 5>        xfer_eq_gains{};
    std::array<float, kNumBins> xfer_eq_bins{};
    bool                        xfer_has_bins{false};
    std::array<float, 5>        mt_eq_gains{};
    std::array<float, kNumBins> mt_eq_bins{};
    bool                        mt_has_bins{false};

    // Staging for JSON build (main thread only).
    std::array<std::array<float, kFFT>, kNumStages> mt_frames{};

    void init() {
        for (int i = 0; i < kFFT; ++i)
            hann[i] = 0.5f * (1.f - std::cos(2.f * static_cast<float>(M_PI) * i / kFFT));
        for (int i = 0; i < kDisp; ++i)
            disp_hz[i] = kFlo * std::pow(kFhi / kFlo, float(i) / (kDisp - 1));
        for (auto& row : ema) row.fill(0.f);
    }

    // Audio thread: accumulate n mono (or averaged stereo) samples for chain pos.
    void push(int pos, const float* L, const float* R, uint32_t n_ch, int n) {
        auto& a = accum[pos];
        if (a.fill >= kFFT) return;
        const int take = std::min(n, kFFT - a.fill);
        float* dst = a.buf.data() + a.fill;
        if (n_ch >= 2) {
            for (int i = 0; i < take; ++i) dst[i] = 0.5f * (L[i] + R[i]);
        } else {
            std::copy_n(L, take, dst);
        }
        a.fill += take;
    }

    // Audio thread: latch the 5 LSTM EQ band gains (dB) for the next transfer.
    void set_eq_gains(const float* gains_db_5) {
        std::copy_n(gains_db_5, 5, xfer_eq_gains.data());
    }
    // Audio thread: latch 50-point bin gains (dB) for SpectralMask view.
    void set_eq_bins(const float* gains_db) {
        std::copy_n(gains_db, kNumBins, xfer_eq_bins.data());
        xfer_has_bins = true;
    }
    void clear_eq_bins() { xfer_has_bins = false; }

    // Audio thread: when all accumulators are full, try to hand off to main thread.
    // Returns true when a transfer was attempted (whether or not the lock was acquired).
    bool advance_and_transfer() {
        if (accum[0].fill < kFFT) return false;
        if (xfer_mtx.try_lock()) {
            for (int p = 0; p < kNumStages; ++p) xfer_frames[p] = accum[p].buf;
            xfer_eq_gains_snap   = xfer_eq_gains;
            xfer_eq_bins_snap    = xfer_eq_bins;
            xfer_has_bins_snap   = xfer_has_bins;
            xfer_ready = true;
            xfer_mtx.unlock();
        }
        for (auto& a : accum) a.fill = 0;
        return true;
    }

    // Main thread: process pending transfer; returns true if new data was ready.
    bool process_if_ready(double sample_rate) {
        {
            std::lock_guard<std::mutex> lk(xfer_mtx);
            if (!xfer_ready) return false;
            mt_frames     = xfer_frames;
            mt_eq_gains   = xfer_eq_gains_snap;
            mt_eq_bins    = xfer_eq_bins_snap;
            mt_has_bins   = xfer_has_bins_snap;
            xfer_ready    = false;
        }
        const float sr = static_cast<float>(sample_rate);
        for (int pos = 0; pos < kNumStages; ++pos) {
            for (int i = 0; i < kFFT; ++i)
                windowed[i] = mt_frames[pos][i] * hann[i];
            for (int b = 0; b < kDisp; ++b) {
                const float bin_f = disp_hz[b] * kFFT / sr;
                const float mag   = goertzel(windowed.data(), kFFT, bin_f);
                ema[pos][b] = kAlpha * ema[pos][b] + (1.f - kAlpha) * mag;
            }
        }
        return true;
    }

    // Main thread: build the JS call string for the WebView.
    std::string build_js(const std::array<int, kNumStages>& order) const {
        std::string s;
        s.reserve(8192);
        s = "axonSpectrum({\"order\":[";
        for (int i = 0; i < kNumStages; ++i) { if (i) s += ','; s += std::to_string(order[i]); }
        s += "],\"db\":[";
        char buf[16];
        for (int pos = 0; pos < kNumStages; ++pos) {
            if (pos) s += ',';
            s += '[';
            for (int b = 0; b < kDisp; ++b) {
                if (b) s += ',';
                snprintf(buf, sizeof(buf), "%.1f",
                         20.f * std::log10(std::max(ema[pos][b], 1e-9f)));
                s += buf;
            }
            s += ']';
        }
        // 5 LSTM EQ band gains in dB so JS can draw the filter response curve.
        s += "],\"eq\":[";
        for (int b = 0; b < 5; ++b) {
            if (b) s += ',';
            snprintf(buf, sizeof(buf), "%.2f", mt_eq_gains[b]);
            s += buf;
        }
        // 50-point bin gains (SpectralMask only) or null (PEQ classes).
        s += "],\"eq_bins\":";
        if (mt_has_bins) {
            s += '[';
            for (int b = 0; b < kNumBins; ++b) {
                if (b) s += ',';
                snprintf(buf, sizeof(buf), "%.2f", mt_eq_bins[b]);
                s += buf;
            }
            s += ']';
        } else {
            s += "null";
        }
        s += "});";
        return s;
    }

private:
    // Goertzel algorithm for the magnitude at a single fractional bin.
    static float goertzel(const float* x, int N, float bin_f) {
        const int    k     = static_cast<int>(std::round(bin_f));
        const double w     = 2.0 * M_PI * static_cast<double>(std::clamp(k, 0, N/2));
                           // N samples in denominator:
        const double coeff = 2.0 * std::cos(w / N);
        double s1 = 0.0, s2 = 0.0;
        for (int n = 0; n < N; ++n) {
            const double s0 = x[n] + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        const float power = static_cast<float>(s1*s1 + s2*s2 - coeff*s1*s2);
        return std::sqrt(std::max(power, 0.f)) * (2.f / N);
    }
};

// ---------------------------------------------------------------------------
// Module-global state (loaded once at module init)
// ---------------------------------------------------------------------------

struct ModuleState {
    CompositeMeta              axon_meta;
    // One PluginMeta per auto-EQ class. Indexed by axon_meta.auto_eq.class_order;
    // the lookup map mirrors the same data keyed by class name for convenience.
    std::vector<PluginMeta>                              autoeq_metas;
    std::unordered_map<std::string, std::size_t>         autoeq_class_index;
    PluginMeta                 sat_meta;
    PluginMeta                 ssl_comp_meta;          // optional; loaded if
                                                       // axon_meta.sub_bundles
                                                       // has "ssl_comp"
    bool                       ssl_comp_loaded{false};
    std::string                bundle_dir;            // .../Axon.clap/Contents
    std::string                resources_dir;         // .../Contents/Resources
    std::string                plugin_id_str;         // "com.nablafx.<model_id>"
    clap_plugin_descriptor_t   descriptor{};
    std::vector<const char*>   feature_ptrs;
    std::array<const char*, 3> feature_storage{};
    std::unique_ptr<Ort::Env>  ort_env;
    // Pulled out of sat_meta once at load.
    RationalAParams            sat_rational;
    // Per-class DSP-block payload, parsed once at load. Every class declares
    // ``spectral_mask_eq`` as its dsp_blocks[0]; held here so the audio
    // thread can read num_control_params without re-parsing meta.
    std::vector<DspBlockSpec>  autoeq_dsp_per_class;
    int                        autoeq_default_idx{0};
};

static ModuleState* g_state = nullptr;

static std::string find_bundle_contents_() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&find_bundle_contents_), &info) == 0
        || !info.dli_fname) {
        return {};
    }
    fs::path dylib = info.dli_fname;
    // .clap/Contents/MacOS/<dylib> → .clap/Contents
    return dylib.parent_path().parent_path().string();
}

static void populate_descriptor_(ModuleState& st) {
    // Build a short plugin ID from effect_name (model_id can be 300+ chars,
    // which overflows fixed-size ID buffers in some CLAP hosts).
    std::string short_name = st.axon_meta.effect_name;
    std::transform(short_name.begin(), short_name.end(), short_name.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    for (auto& c : short_name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') c = '_';
    st.plugin_id_str = "com.nablafx." + short_name;

    st.feature_storage[0] = CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
    st.feature_storage[1] = CLAP_PLUGIN_FEATURE_MASTERING;
    st.feature_storage[2] = nullptr;
    st.feature_ptrs.assign(st.feature_storage.begin(), st.feature_storage.end());

    st.descriptor.clap_version = CLAP_VERSION_INIT;
    st.descriptor.id           = st.plugin_id_str.c_str();
    st.descriptor.name         = st.axon_meta.effect_name.c_str();
    st.descriptor.vendor       = "nablafx";
    st.descriptor.url          = "https://github.com/mcomunita/nablafx";
    st.descriptor.manual_url   = "";
    st.descriptor.support_url  = "";
    st.descriptor.version      = "1.0.0";
    st.descriptor.description  = "Axon — adaptive mastering chain (auto-EQ + saturator + bus comp + bass mono + limiter + ceiling)";
    st.descriptor.features     = st.feature_ptrs.data();
}

// ---------------------------------------------------------------------------
// Per-channel chain state. One of these per audio channel.
// ---------------------------------------------------------------------------

struct StateBuf {
    std::vector<int64_t> shape;
    std::vector<float>   data;
};

class OrtMiniSession {
    // Thin wrapper around an Ort::Session for fixed-shape audio + state I/O.
    // Owns the input/output state buffers; you set audio + controls (if any),
    // then run() reads/writes state, and call swap() to make this run's
    // outputs the next run's inputs.
public:
    OrtMiniSession(Ort::Env& env, const std::string& model_path, const PluginMeta& meta)
        : env_(env), cpu_(Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU)),
          meta_(meta) {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetInterOpNumThreads(1);
        opts.SetExecutionMode(ORT_SEQUENTIAL);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), opts);
        for (const auto& nm : meta.input_names)  in_names_owned_.push_back(nm);
        for (const auto& nm : meta.output_names) out_names_owned_.push_back(nm);
        for (auto& s : in_names_owned_)  in_names_.push_back(s.c_str());
        for (auto& s : out_names_owned_) out_names_.push_back(s.c_str());

        // Allocate state buffers and pre-build owned "_in"/"_out" name strings
        // so run() never constructs temporaries whose .c_str() would dangle.
        for (const auto& s : meta.state_tensors) {
            int64_t n = 1;
            for (auto d : s.shape) n *= d;
            in_states_[s.name].shape  = s.shape;
            in_states_[s.name].data.assign(n, 0.0f);
            out_states_[s.name].shape = s.shape;
            out_states_[s.name].data.assign(n, 0.0f);
            state_in_names_owned_.push_back(s.name + "_in");
            state_out_names_owned_.push_back(s.name + "_out");
        }
    }

    StateBuf& in_state(const std::string& name) { return in_states_.at(name); }

    void reset_state() {
        for (auto& [_, b] : in_states_)  std::fill(b.data.begin(), b.data.end(), 0.0f);
        for (auto& [_, b] : out_states_) std::fill(b.data.begin(), b.data.end(), 0.0f);
    }

    // Run with caller-owned audio (and optional controls) buffers. Outputs
    // are written into `audio_out` (and the internal state-out buffers).
    void run(const float* audio_in, int audio_in_len,
             float* audio_out, int audio_out_len,
             const float* controls /*nullable*/, int n_controls,
             const std::string& audio_out_name = "audio_out") {
        std::vector<Ort::Value>      inputs;
        std::vector<const char*>     in_names;
        std::vector<const char*>     out_names;
        in_names.reserve(in_names_.size());
        out_names.reserve(out_names_.size());

        std::array<int64_t, 3> aud_shape{1, 1, audio_in_len};
        inputs.push_back(Ort::Value::CreateTensor<float>(
            cpu_, const_cast<float*>(audio_in), audio_in_len,
            aud_shape.data(), aud_shape.size()));
        in_names.push_back("audio_in");

        std::array<int64_t, 2> ctl_shape{1, n_controls};
        if (n_controls > 0) {
            inputs.push_back(Ort::Value::CreateTensor<float>(
                cpu_, const_cast<float*>(controls), n_controls,
                ctl_shape.data(), ctl_shape.size()));
            in_names.push_back("controls");
        }

        // Add state inputs in the order the meta declared them.
        for (std::size_t si = 0; si < meta_.state_tensors.size(); ++si) {
            auto& buf = in_states_[meta_.state_tensors[si].name];
            inputs.push_back(Ort::Value::CreateTensor<float>(
                cpu_, buf.data.data(), static_cast<int64_t>(buf.data.size()),
                buf.shape.data(), buf.shape.size()));
            in_names.push_back(state_in_names_owned_[si].c_str());
        }

        // Output: audio first, then states in declared order.
        for (const auto& nm : out_names_) out_names.push_back(nm);

        auto outs = session_->Run(Ort::RunOptions{nullptr},
                                  in_names.data(), inputs.data(), inputs.size(),
                                  out_names.data(), out_names.size());

        // Copy audio out (must match the requested length).
        // The first output is named audio_out_name; locate it by index.
        std::size_t audio_out_idx = 0;
        for (std::size_t i = 0; i < out_names_owned_.size(); ++i) {
            if (out_names_owned_[i] == audio_out_name) {
                audio_out_idx = i; break;
            }
        }
        const float* aud_out = outs[audio_out_idx].GetTensorData<float>();
        // The actual ORT output length may be SHORTER than audio_in_len —
        // streaming-mode TCN export trims (rf-1) samples (no internal
        // pre-padding; output_len = input_len - (rf-1)). Clamp to the real
        // tensor element count to avoid reading past the end (which would
        // emit garbage memory and produce a hop-rate flutter on the wet).
        const auto out_info = outs[audio_out_idx].GetTensorTypeAndShapeInfo();
        const int64_t actual_len =
            static_cast<int64_t>(out_info.GetElementCount());
        const int64_t copy_len =
            std::min<int64_t>(audio_out_len, actual_len);
        std::copy_n(aud_out, copy_len, audio_out);
        // If the caller asked for more than the model produced, zero-fill the
        // tail so callers that don't size-check at least see silence rather
        // than uninitialised memory.
        if (copy_len < audio_out_len) {
            std::fill(audio_out + copy_len,
                      audio_out + audio_out_len, 0.0f);
        }

        // Read back states by output-name (always "<state>_out").
        for (std::size_t si = 0; si < meta_.state_tensors.size(); ++si) {
            const std::string& out_name = state_out_names_owned_[si];
            std::size_t idx = 0;
            for (std::size_t i = 0; i < out_names_owned_.size(); ++i) {
                if (out_names_owned_[i] == out_name) { idx = i; break; }
            }
            const float* p = outs[idx].GetTensorData<float>();
            const std::string& sname = meta_.state_tensors[si].name;
            std::copy_n(p, out_states_[sname].data.size(), out_states_[sname].data.begin());
        }
    }

    void swap_state() {
        for (const auto& s : meta_.state_tensors) {
            std::swap(in_states_[s.name].data, out_states_[s.name].data);
        }
    }

    // Run-arbitrary variant for the auto-EQ controller, where the audio
    // output channel name is "params_proc_0" and represents [1, 15, T]
    // sigmoid params instead of audio. We expose only the first sample
    // (all samples in a block are identical post-repeat_interleave).
    void run_controller(const float* audio_in, int audio_in_len,
                        float* params_out_first, int params_out_count) {
        std::vector<Ort::Value>  inputs;
        std::vector<const char*> in_names;
        std::vector<const char*> out_names;

        std::array<int64_t, 3> aud_shape{1, 1, audio_in_len};
        inputs.push_back(Ort::Value::CreateTensor<float>(
            cpu_, const_cast<float*>(audio_in), audio_in_len,
            aud_shape.data(), aud_shape.size()));
        in_names.push_back("audio_in");

        for (std::size_t si = 0; si < meta_.state_tensors.size(); ++si) {
            auto& buf = in_states_[meta_.state_tensors[si].name];
            inputs.push_back(Ort::Value::CreateTensor<float>(
                cpu_, buf.data.data(), static_cast<int64_t>(buf.data.size()),
                buf.shape.data(), buf.shape.size()));
            in_names.push_back(state_in_names_owned_[si].c_str());
        }
        for (const auto& nm : out_names_) out_names.push_back(nm);

        auto outs = session_->Run(Ort::RunOptions{nullptr},
                                  in_names.data(), inputs.data(), inputs.size(),
                                  out_names.data(), out_names.size());

        // First output is the params tensor [1, 15, T]. We want the first
        // sample (all are identical within a block per the controller's
        // repeat_interleave structure).
        const float* p = outs[0].GetTensorData<float>();
        // Stride: T = audio_in_len, channel-major contiguous so element
        // [channel c, sample 0] is at offset c * T.
        for (int c = 0; c < params_out_count; ++c) {
            params_out_first[c] = p[c * audio_in_len + 0];
        }

        for (std::size_t si = 0; si < meta_.state_tensors.size(); ++si) {
            const std::string& out_name = state_out_names_owned_[si];
            std::size_t idx = 0;
            for (std::size_t i = 0; i < out_names_owned_.size(); ++i) {
                if (out_names_owned_[i] == out_name) { idx = i; break; }
            }
            const float* sp = outs[idx].GetTensorData<float>();
            const std::string& sname = meta_.state_tensors[si].name;
            std::copy_n(sp, out_states_[sname].data.size(), out_states_[sname].data.begin());
        }
    }

private:
    Ort::Env&                     env_;
    Ort::MemoryInfo               cpu_;
    PluginMeta                    meta_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<std::string>      in_names_owned_;
    std::vector<std::string>      out_names_owned_;
    std::vector<std::string>      state_in_names_owned_;   // "<base>_in" per state tensor
    std::vector<std::string>      state_out_names_owned_;  // "<base>_out" per state tensor
    std::vector<const char*>      in_names_;
    std::vector<const char*>      out_names_;
    std::unordered_map<std::string, StateBuf> in_states_;
    std::unordered_map<std::string, StateBuf> out_states_;
};

struct ChannelChain {
    // Per-channel stage instances. TruePeakCeiling runs per-channel.
    // One ORT session per auto-EQ class (bass/drums/vocals/other/full_mix);
    // CLS picks which one is active. Inactive sessions hold zeroed state
    // so a class switch starts the LSTM from a neutral init and avoids
    // bleeding stale activations from a different class's signal.
    std::vector<std::unique_ptr<OrtMiniSession>> autoeq_ort_per_class;
    // Per-class SpectralMaskEq instance. Every class shares the same kind
    // now, but each class still gets its own instance so the per-class
    // mask-smoother state doesn't bleed across class switches.
    std::vector<std::unique_ptr<SpectralMaskEq>>    autoeq_spec_per_class;
    // Per-class zero-latency IIR-filterbank renderer — the alternate to the STFT
    // mask, selected by EQ_RENDER. Same per-band [0,1] contract; minimum-phase
    // biquad cascade, no STFT framing, no pre-ring.
    std::vector<std::unique_ptr<nablafx::IirFilterbankEq>> autoeq_iir_per_class;
    // Peak-hold envelope follower for auto-EQ controller input normalization.
    // Training normalized peak per ~10 s segment; using a per-128-block peak at
    // runtime collapsed the LSTM's input distribution. Attack-instant /
    // decay-slow tracking gives a stable scale across blocks.
    float                                  autoeq_peak_env{0.f};
    // Deterministic Auto-EQ controller (the C1→C2 cascade). Class-agnostic, so
    // one instance per channel drives the SAME SpectralMaskEq renderer when the
    // EQ_ENGINE param selects "Adaptive" instead of the per-class LSTM.
    nablafx::AdaptiveEqController          adaptive_eq;
    RationalA                              saturator;
    // 1st-order HPF for bass-preserved saturation (bilinear transform).
    float sat_hpf_fc{-1.f};               // cached cutoff; -1 = stale
    float sat_hpf_b0{0.f}, sat_hpf_b1{0.f}, sat_hpf_a1{0.f};
    float sat_hpf_x1{0.f}, sat_hpf_y1{0.f};
    // 1st-order LPF for treble-limited saturation (bilinear transform).
    float sat_lpf_fc{-1.f};
    float sat_lpf_b0{0.f}, sat_lpf_b1{0.f}, sat_lpf_a1{0.f};
    float sat_lpf_x1{0.f}, sat_lpf_y1{0.f};
    // SSL-style bus comp (separate stage from LA-2A). Stateless long-RF
    // causal TCN: needs a trace_len-sized input ring per channel because the
    // ORT call expects all RF samples of context per invocation. Allocated
    // only when the ssl_comp sub-bundle is shipped; otherwise null and the
    // SslComp stage is a passthrough.
    std::unique_ptr<OrtMiniSession>        ssl_comp_ort;
    std::vector<float>                     ssl_comp_in_ring;     // [trace_len]
    std::vector<float>                     ssl_comp_out_buf;     // [trace_len]
    // Hop accumulation: re-running the full TCN forward pass every
    // kBlockSize=128 samples blows the audio thread's deadline at long RF.
    // Accumulate kSslHop input samples, then run ORT once and play out the
    // resulting kSslHop samples over (kSslHop / kBlockSize) host calls. The
    // model still sees trace_len of context per call; we just call it less
    // often. Adds (kSslHop - kBlockSize) samples of latency.
    std::vector<float>                     ssl_comp_in_accum;    // [kSslHop]
    int                                    ssl_comp_in_fill{0};
    std::vector<float>                     ssl_comp_out_queue;   // [kSslHop]
    int                                    ssl_comp_out_avail{0};
    int                                    ssl_comp_out_read{0};
    // Dry delay ring: holds (kSslHop - kBlockSize) samples of dry audio so
    // the wet/dry blend is sample-aligned. Without this, blending a delayed
    // wet with the current dry produces hop-rate comb-filter flutter.
    std::vector<float>                     ssl_comp_dry_delay;
    int                                    ssl_comp_dry_write{0};
    // SSL 9000 J channel EQ — native biquad cascade, per channel (holds its own
    // z-state). Zero latency; coeffs recomputed at host SR inside set_params.
    nablafx::SslChannelEq                  ssl_eq;
    // Long-smoothed assist-band gains (ramp toward the main-thread solve; ~2.7 s
    // one-pole per block so a re-solve can't zipper).
    std::array<float, nablafx::SslChannelEq::kNumAssist> ssl_asg_smooth{};
    TruePeakCeiling                        ceiling;

    // 128-sample accumulator: kBlockSize input samples in, then a chain pass,
    // then kBlockSize output samples ready. Output ring fills before any reads
    // so the first kBlockSize host samples produce silence (latency reported
    // to the host so DAWs compensate).
    std::array<float, kBlockSize> in_buf{};
    int                           in_fill = 0;

    std::array<float, kBlockSize> out_buf{};
    int                           out_avail = 0;
    int                           out_read  = 0;

    // Raw-input delay FIFO for the internal Bypass (audition the unprocessed
    // signal). Pushed as input is consumed, popped as output is drained, so it
    // naturally lags the output by exactly the plugin latency — toggling Bypass
    // stays time-aligned (and host latency compensation matches the wet path).
    std::vector<float>            bypass_fifo;
};

// ---------------------------------------------------------------------------
// Per-instance state
// ---------------------------------------------------------------------------

struct Plugin {
    clap_plugin_t      plugin{};
    const clap_host_t* host{nullptr};

    const CompositeMeta* meta{nullptr};
    int                  channels{2};
    double               sample_rate{};
    bool                 activated{false};

    std::vector<float>   control_values;

    // Decay coefficient for the per-channel auto-EQ peak envelope follower.
    // Computed at activate from sample_rate so the time constant stays at
    // ~500 ms regardless of host sr. Attack is instantaneous.
    float                autoeq_env_decay{0.f};

    // Multiband adaptive limiter (stereo, initialized at activate).
    nablafx::MelLimiter  mel_limiter;

    // Bass mono-maker (stereo; collapses width below a cutoff).
    nablafx::BassMono    bass_mono;

    // HARMONICS: two CLEAN narrowband multiband exciters in series. Warmth
    // excites the low-mids (even/2nd → body); Presence the highs (even+odd →
    // air). Narrowband-per-knob keeps intermodulation low (a wide waveshaper
    // makes broadband IMD "noise"). Each knob is the added-harmonic level.
    nablafx::MultibandExciter exc_warm;
    nablafx::MultibandExciter exc_pres;

    // DIRECT wet-spectrum meters for the two-zone overlay. Each is fed (read-only)
    // the mono wet CONTRIBUTION of its band, so the UI draws exactly what each
    // knob adds. Updated from the Harmonics stage; snapshotted to *_db under a
    // benign meter race (same pattern as lim_brick).
    nablafx::ExciterSpectrum exc_warm_spectrum;
    nablafx::ExciterSpectrum exc_pres_spectrum;
    std::array<float, nablafx::ExciterSpectrum::kNumBins> exc_warm_db{};
    std::array<float, nablafx::ExciterSpectrum::kNumBins> exc_pres_db{};
    bool exc_spec_active{false};

    // Transparent mastering room reverb (single stereo 8-line FDN network).
    nablafx::Reverb      reverb;

    // Transparent M/S stereo widener (single stereo instance; frequency-dependent
    // side gain — mono-compatible by construction). Like the reverb, NOT per-chain.
    nablafx::Widener     widener;

    // Auto gain (level-matched bypass) — drives output LUFS toward input LUFS.
    nablafx::AutoGain    auto_gain;

    // Internal-bypass dry FIFO indices (shared across channels; advance in lock-
    // step). Ring size is fixed and comfortably larger than any plugin latency.
    static constexpr int kBypassRing = 1 << 15;   // 32768
    int bypass_w{0}, bypass_r{0};

    // Processor ordering — driven by GUI drag-and-drop. All stages reorderable.
    // Default: BassMono → SslEq → AutoEQ → Reverb → Widener → SslComp →
    // MelLimiter. SslEq (3) sits before AutoEQ so its assist bands can pre-EQ
    // what the Auto-EQ sees (coupling). The reverb sits AFTER BassMono (so its
    // bass is already tightened) and BEFORE the limiter, so the limiter still
    // catches any reverb peaks.
    std::array<int, kNumStages> processor_order{6, 3, 1, 8, 9, 4, 5};   // Harmonics(7) disabled

    // Active auto-EQ class index (into ModuleState::autoeq_metas /
    // axon_meta.auto_eq.class_order). Updated from the audio thread when the
    // CLS control changes, so the AutoEQ stage routes through
    // chains[ch].autoeq_ort_per_class[active_autoeq_cls].
    int active_autoeq_cls{0};

    // GUI → audio-thread param queue (try_lock on audio thread, never blocks).
    std::mutex                       param_mutex;
    std::vector<std::pair<int,float>> param_queue;

    // GUI → audio-thread order change.
    std::mutex               order_mutex;
    bool                     order_pending{false};
    std::array<int,kNumStages> pending_order{6,3,1,8,9,4,5};   // Harmonics(7) disabled

    // CLAP GUI handle (main thread only).
    AxonGUIState* gui_state{nullptr};

    // Dynamic latency tracking.
    // current_latency is written by the audio thread and read by latency_get
    // (main thread). latency_needs_notify is set by the audio thread and
    // cleared by on_main_thread after calling host_latency_ext->changed().
    std::atomic<uint32_t>           current_latency{0};
    std::atomic<bool>               latency_needs_notify{false};
    const clap_host_latency_t*      host_latency_ext{nullptr};

    // Spectrum analyzer (audio thread accumulates, main thread computes + renders).
    SpectrumAnalyzer spectrum;

    // Auto-EQ -> SSL coupling (static solve). The main thread fits the 6 assist-band
    // gains to the auto-EQ's resolved curve (spectrum.mt_eq_bins) and publishes them
    // via a seqlock (even generation = stable); the SslEq audio stage ramps toward
    // them. See docs/ssl_eq_coupling.md — fixed beats dynamic, so this is slow/static.
    nablafx::SslEqSolver ssl_solver{nablafx::SslEqSolver::assist()};
    std::atomic<uint64_t> ssl_asg_gen{0};
    std::array<float, nablafx::SslChannelEq::kNumAssist> ssl_asg_published{};
    std::array<float, nablafx::SslChannelEq::kNumAssist> ssl_asg_accum{};  // main-thread accumulator
    bool ssl_recal_prev{false};
    bool ssl_reset_prev{false};
    int  ssl_solve_timer{0};
    // Main-thread scratch SSL EQ used only to compute the display curve (manual
    // bands + the published assist gains) so the UI can show the SSL's TOTAL
    // contribution, incl. the coupling assist bands. Never touched by audio.
    nablafx::SslChannelEq ssl_viz_eq;

    // In/out level meters (audio thread updates; published to UI via atomics).
    nablafx::LoudnessMeter  meter_in;
    nablafx::LoudnessMeter  meter_out;
    std::atomic<float> m_in_lufs_s{-120.f}, m_in_lufs_m{-120.f},
                       m_in_rms{-120.f},    m_in_peak{-120.f};
    std::atomic<float> m_out_lufs_s{-120.f}, m_out_lufs_m{-120.f},
                       m_out_rms{-120.f},    m_out_peak{-120.f};

    // Limiter band visualization (audio thread snapshots under try_lock).
    std::mutex lim_mtx;
    std::array<float, nablafx::MelLimiter::num_bands()> lim_levels{}, lim_gains{}, lim_centers{};
    float lim_ceiling{1.f};
    float lim_brick{1.f};      // brickwall gain (1 = no peak limiting)
    bool  lim_active{false};

    // Bus Comp "crunch" telemetry (plain float members set on the audio thread,
    // read on the main thread — a benign meter race, same as lim_brick). The
    // bus comp is a learned TCN with no readable internal gain reduction, AND it
    // phase-rotates the signal, so a residual rms(wet-dry) is pinned near 0 dB.
    // Instead we measure phase-invariant magnitude-squared coherence between the
    // model input (dry) and output (wet) — see CoherenceDistortion:
    //   bc_distortion_db = 10*log10(1 - γ²), how much the model is altering the
    //                      signal in a phase-invariant way (-48..0 dB)
    //   bc_crest_red_db  = crest(dry) - crest(wet) = dynamics being squashed (≥0)
    float bc_distortion_db{-48.f};
    float bc_crest_red_db{0.f};
    bool  bc_active{false};
    // Coherence analyzer: averages dry-vs-wet spectra over ~200 ms to extract a
    // phase-invariant distortion measure. Allocated in activate, fed read-only
    // from the SslComp tap (mono sum of dry_aligned / wet_a).
    nablafx::CoherenceDistortion bc_coherence;

    std::vector<ChannelChain> chains;
};

// ---------------------------------------------------------------------------
// CLAP extension: audio ports — 1 stereo input, 1 stereo output
// ---------------------------------------------------------------------------

static uint32_t audio_ports_count(const clap_plugin_t*, bool /*is_input*/) { return 1; }

static bool audio_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                            clap_audio_port_info_t* info) {
    if (index != 0) return false;
    info->id            = is_input ? 0 : 1;
    std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
    info->channel_count = 2;
    info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type     = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t s_ext_audio_ports = {audio_ports_count, audio_ports_get};

// ---------------------------------------------------------------------------
// CLAP extension: params (AMT, TRM)
// ---------------------------------------------------------------------------

static uint32_t params_count(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    return static_cast<uint32_t>(plug->meta->controls.size());
}

static bool params_get_info(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    if (index >= plug->meta->controls.size()) return false;
    const auto& c = plug->meta->controls[index];
    info->id        = param_id_for(plug->meta->effect_name, c.id);
    info->flags     = CLAP_PARAM_IS_AUTOMATABLE;
    info->cookie    = nullptr;
    info->min_value = c.min;
    info->max_value = c.max;
    info->default_value = c.def;
    std::snprintf(info->name,   sizeof(info->name),   "%s", c.name.c_str());
    std::snprintf(info->module, sizeof(info->module), "%s", "");
    return true;
}

static bool params_get_value(const clap_plugin_t* p, clap_id id, double* value) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    for (size_t i = 0; i < plug->meta->controls.size(); ++i) {
        if (param_id_for(plug->meta->effect_name, plug->meta->controls[i].id) == id) {
            *value = plug->control_values[i];
            return true;
        }
    }
    return false;
}

static bool params_value_to_text(const clap_plugin_t* p, clap_id id, double value, char* out, uint32_t out_size) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    // CLS displays the class name instead of the integer index.
    for (size_t i = 0; i < plug->meta->controls.size(); ++i) {
        if (param_id_for(plug->meta->effect_name, plug->meta->controls[i].id) == id
            && plug->meta->controls[i].id == "CLS") {
            const auto& classes = g_state->axon_meta.auto_eq.class_order;
            int idx = std::clamp(static_cast<int>(std::lround(value)),
                                 0, static_cast<int>(classes.size()) - 1);
            std::snprintf(out, out_size, "%s", classes[idx].c_str());
            return true;
        }
    }
    std::snprintf(out, out_size, "%.3f", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t* p, clap_id id, const char* text, double* out) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    // CLS accepts a class name and converts it to the canonical index.
    for (size_t i = 0; i < plug->meta->controls.size(); ++i) {
        if (param_id_for(plug->meta->effect_name, plug->meta->controls[i].id) == id
            && plug->meta->controls[i].id == "CLS") {
            const auto& classes = g_state->axon_meta.auto_eq.class_order;
            for (size_t k = 0; k < classes.size(); ++k) {
                if (classes[k] == text) {
                    *out = static_cast<double>(k);
                    return true;
                }
            }
            // Fall through to numeric parse if the text isn't a class name.
        }
    }
    char* end = nullptr;
    double v = std::strtod(text, &end);
    if (end == text) return false;
    *out = v;
    return true;
}

static void params_flush(const clap_plugin_t*, const clap_input_events_t*, const clap_output_events_t*) {}

static const clap_plugin_params_t s_ext_params = {
    params_count, params_get_info, params_get_value, params_value_to_text,
    params_text_to_value, params_flush,
};

// ---------------------------------------------------------------------------
// CLAP extension: latency
// ---------------------------------------------------------------------------

// Computes the true end-to-end latency for the current parameter state.
// Called from the audio thread (after param drain) and from activate.
// Sources:
//   kBlockSize          — input accumulator always present
//   SpectralMaskEq      — n_fft - hop, only when EQ wet > 0 and class is spectral
//   SSL bus comp        — kSslHop - kBlockSize, only when SSC > 0 and loaded
//   Exciter             — oversampler FIR group delay, only when EXC_AMT > 0
//   TruePeakCeiling     — lookahead, always present once activated
static uint32_t compute_latency_(const Plugin& plug) {
    if (plug.chains.empty() || !g_state) return 0;

    uint32_t lat = kBlockSize;
    lat += static_cast<uint32_t>(plug.chains[0].ceiling.latency_samples());

    float eq_wet = 0.f, ssc_wet = 0.f, ml_wet = 0.f, eq_render = 0.f;
    int   cls_idx = 0;
    for (size_t i = 0; i < plug.meta->controls.size(); ++i) {
        const auto& c = plug.meta->controls[i];
        const float v = plug.control_values[i];
        if      (c.id == "EQ")        eq_wet    = v;
        else if (c.id == "SSC")       ssc_wet   = v;
        else if (c.id == "CLS")       cls_idx   = static_cast<int>(std::lround(v));
        else if (c.id == "MLI")       ml_wet    = v;
        else if (c.id == "EQ_RENDER") eq_render = v;
    }

    // The STFT mask renderer adds n_fft latency; the IIR-bank renderer is
    // zero-latency. This recompute fires on every param change (and notifies the
    // host), so toggling EQ_RENDER re-PDCs exactly like toggling EQ on/off.
    if (eq_wet > 0.f && eq_render < 0.5f && !g_state->autoeq_dsp_per_class.empty()) {
        const int n_cls = static_cast<int>(g_state->autoeq_dsp_per_class.size());
        cls_idx = std::clamp(cls_idx, 0, n_cls - 1);
        if (g_state->autoeq_dsp_per_class[cls_idx].kind == "spectral_mask_eq") {
            const auto& sp = std::get<SpectralMaskEqParams>(
                g_state->autoeq_dsp_per_class[cls_idx].params);
            lat += static_cast<uint32_t>(sp.n_fft);
        }
    }

    if (g_state->ssl_comp_loaded && ssc_wet > 0.f)
        lat += static_cast<uint32_t>(kSslHop - kBlockSize);

    if (ml_wet > 0.f)
        lat += static_cast<uint32_t>(nablafx::MelLimiter::kLatency);

    // (Harmonics/Exciter is disabled — not in processor_order — so it adds no
    // latency. Its FIR-group-delay contribution returns when stage 7 is re-added.)

    return lat;
}

static uint32_t latency_get(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    return plug->current_latency.load(std::memory_order_relaxed);
}

static const clap_plugin_latency_t s_ext_latency = {latency_get};

// ---------------------------------------------------------------------------
// CLAP extension: state (save / load)
// ---------------------------------------------------------------------------

// Forward declaration — defined with the GUI extension below.
static void gui_send_full_state_(Plugin* plug);

static bool state_save(const clap_plugin_t* p, const clap_ostream_t* stream) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    nlohmann::json j;
    j["version"] = 2;
    for (size_t i = 0; i < plug->meta->controls.size(); ++i)
        j["controls"][plug->meta->controls[i].id] = plug->control_values[i];
    auto& jo = j["processor_order"];
    for (int i = 0; i < kNumStages; ++i) jo.push_back(plug->processor_order[i]);
    std::string txt = j.dump();
    int64_t written = stream->write(stream, txt.data(), txt.size());
    return written == static_cast<int64_t>(txt.size());
}

static bool state_load(const clap_plugin_t* p, const clap_istream_t* stream) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    std::string txt;
    char buf[4096];
    int64_t n;
    while ((n = stream->read(stream, buf, sizeof(buf))) > 0)
        txt.append(buf, static_cast<size_t>(n));
    if (n < 0) return false;
    try {
        auto j = nlohmann::json::parse(txt);
        auto& jc = j.at("controls");
        for (size_t i = 0; i < plug->meta->controls.size(); ++i) {
            const auto& id = plug->meta->controls[i].id;
            if (jc.contains(id))
                plug->control_values[i] = jc.at(id).get<float>();
        }
        if (j.contains("processor_order")) {
            auto& jo = j.at("processor_order");
            if (jo.is_array() && static_cast<int>(jo.size()) == kNumStages) {
                for (int i = 0; i < kNumStages; ++i)
                    plug->processor_order[i] = jo[i].get<int>();
            }
        }
    } catch (...) { return false; }

    // Tell the host that all parameter values changed.
    const auto* host_params = static_cast<const clap_host_params_t*>(
        plug->host->get_extension(plug->host, CLAP_EXT_PARAMS));
    if (host_params && host_params->rescan)
        host_params->rescan(plug->host, CLAP_PARAM_RESCAN_VALUES);
    // If the GUI is already open, push the restored state to it immediately.
    if (plug->gui_state)
        gui_send_full_state_(plug);
    return true;
}

static const clap_plugin_state_t s_ext_state = {state_save, state_load};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static bool plugin_init(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    plug->control_values.resize(plug->meta->controls.size());
    for (size_t i = 0; i < plug->meta->controls.size(); ++i)
        plug->control_values[i] = plug->meta->controls[i].def;
    plug->host_latency_ext = static_cast<const clap_host_latency_t*>(
        plug->host->get_extension(plug->host, CLAP_EXT_LATENCY));
    return true;
}
static void plugin_destroy(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    if (plug->gui_state) { axon_gui_destroy(plug->gui_state); plug->gui_state = nullptr; }
    delete plug;
}

static bool plugin_activate(const clap_plugin_t* p, double sample_rate,
                            uint32_t /*min_frames*/, uint32_t /*max_frames*/) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    plug->sample_rate = sample_rate;
    // ~500 ms peak-envelope decay, evaluated once per kBlockSize-sample block.
    {
        constexpr float kEnvTauSeconds = 0.5f;
        const float blocks_per_tau = (static_cast<float>(sample_rate) * kEnvTauSeconds)
                                     / static_cast<float>(kBlockSize);
        plug->autoeq_env_decay = std::exp(-1.0f / std::max(blocks_per_tau, 1.0f));
    }
    plug->mel_limiter.init(static_cast<int>(sample_rate));

    // Seed active class from CLS control; clamp to a valid class index.
    {
        const auto& classes = g_state->axon_meta.auto_eq.class_order;
        int cls_idx = g_state->autoeq_default_idx;
        for (size_t i = 0; i < plug->meta->controls.size(); ++i) {
            if (plug->meta->controls[i].id == "CLS") {
                cls_idx = static_cast<int>(std::lround(plug->control_values[i]));
                break;
            }
        }
        cls_idx = std::clamp(cls_idx, 0, static_cast<int>(classes.size()) - 1);
        plug->active_autoeq_cls = cls_idx;
    }

    plug->chains.clear();
    plug->chains.resize(plug->channels);
    for (auto& ch : plug->chains) {
        const auto& classes = g_state->axon_meta.auto_eq.class_order;
        ch.autoeq_ort_per_class.clear();
        ch.autoeq_ort_per_class.reserve(classes.size());
        ch.autoeq_spec_per_class.clear();
        ch.autoeq_spec_per_class.resize(classes.size());
        ch.autoeq_iir_per_class.clear();
        ch.autoeq_iir_per_class.resize(classes.size());
        for (size_t i = 0; i < classes.size(); ++i) {
            const std::string& cls = classes[i];
            const std::string& dir = g_state->axon_meta.auto_eq.classes.at(cls);
            ch.autoeq_ort_per_class.push_back(std::make_unique<OrtMiniSession>(
                *g_state->ort_env,
                g_state->resources_dir + "/" + dir + "/model.onnx",
                g_state->autoeq_metas[i]));

            // Every auto-EQ class declares spectral_mask_eq as its
            // dsp_blocks[0]; meta.cpp throws if anything else slips through.
            const auto& dsp = g_state->autoeq_dsp_per_class[i];
            // The audio thread stages control params through a fixed 64-element
            // stack array (eq_params_storage in flush_chain_block_). Fail fast
            // here rather than overflowing that buffer during processing.
            const int n_control =
                std::get<SpectralMaskEqParams>(dsp.params).num_control_params;
            if (n_control > 64) {
                throw std::runtime_error(
                    "auto_eq class '" + cls + "' declares num_control_params=" +
                    std::to_string(n_control) +
                    " which exceeds the 64-element control buffer; "
                    "re-export the bundle or raise eq_params_storage size.");
            }
            ch.autoeq_spec_per_class[i] = std::make_unique<SpectralMaskEq>();
            ch.autoeq_spec_per_class[i]->reset(
                std::get<SpectralMaskEqParams>(dsp.params));
            // Parallel zero-latency IIR renderer for the same band layout.
            ch.autoeq_iir_per_class[i] = std::make_unique<nablafx::IirFilterbankEq>();
            ch.autoeq_iir_per_class[i]->reset(
                std::get<SpectralMaskEqParams>(dsp.params));
        }
        // Deterministic Auto-EQ controller — class-agnostic, configured from the
        // default class's band layout (all classes share the same n_bands/span).
        if (!g_state->autoeq_dsp_per_class.empty()) {
            const int di = std::clamp(g_state->autoeq_default_idx, 0,
                                      (int)g_state->autoeq_dsp_per_class.size() - 1);
            ch.adaptive_eq.reset(std::get<SpectralMaskEqParams>(
                g_state->autoeq_dsp_per_class[di].params));
        }
        ch.saturator.reset(g_state->sat_rational.numerator,
                           g_state->sat_rational.denominator);

        if (g_state->ssl_comp_loaded && g_state->ssl_comp_meta.trace_len > 0) {
            const int N  = g_state->ssl_comp_meta.trace_len;
            const int rf = g_state->ssl_comp_meta.receptive_field;
            // Causal-context safety: each ring shift must preserve at least
            // RF samples of past audio so the model's first hop-output sample
            // sees its full receptive field. Otherwise hop-rate discontinuity.
            if (kSslHop > N - rf) {
                throw std::runtime_error(
                    "ssl_comp: kSslHop=" + std::to_string(kSslHop) +
                    " exceeds trace_len-RF=" + std::to_string(N - rf) +
                    "; would cause hop-rate discontinuity. Either lower "
                    "kSslHop or re-export the bundle with a larger trace_len.");
            }
            ch.ssl_comp_ort = std::make_unique<OrtMiniSession>(
                *g_state->ort_env,
                g_state->resources_dir + "/" +
                    g_state->axon_meta.sub_bundles.at("ssl_comp") + "/model.onnx",
                g_state->ssl_comp_meta);
            ch.ssl_comp_in_ring.assign(N, 0.0f);
            ch.ssl_comp_out_buf.assign(N, 0.0f);
            ch.ssl_comp_in_accum.assign(kSslHop, 0.0f);
            ch.ssl_comp_in_fill = 0;
            ch.ssl_comp_out_queue.assign(kSslHop, 0.0f);
            ch.ssl_comp_out_avail = 0;
            ch.ssl_comp_out_read = 0;
            // Dry delay buffer matches the wet output queue's offset so the
            // blend step sees time-aligned dry. Length = kSslHop - kBlockSize
            // (zero when no accumulation, fits the natural per-block path).
            const int dry_delay_len = kSslHop - kBlockSize;
            ch.ssl_comp_dry_delay.assign(std::max(dry_delay_len, 1), 0.0f);
            ch.ssl_comp_dry_write = 0;
        }

        TruePeakCeiling::Config tcfg{
            /*ceiling_dbtp=*/g_state->axon_meta.ceiling.ceiling_dbtp,
            /*lookahead_ms=*/g_state->axon_meta.ceiling.lookahead_ms,
            /*attack_ms=*/g_state->axon_meta.ceiling.attack_ms,
            /*release_ms=*/g_state->axon_meta.ceiling.release_ms,
        };
        ch.ceiling = TruePeakCeiling(tcfg);
        ch.ceiling.reset(sample_rate);

        // SSL channel EQ: recompute biquad coeffs at the host SR; clear z-state.
        ch.ssl_eq.prepare(sample_rate);
        ch.ssl_asg_smooth.fill(0.f);

        ch.in_fill   = 0;
        ch.out_avail = 0;
        ch.out_read  = 0;
        ch.autoeq_peak_env = 0.f;
        ch.bypass_fifo.assign(Plugin::kBypassRing, 0.f);
    }
    plug->bypass_w = plug->bypass_r = 0;

    plug->spectrum.init();
    plug->bass_mono.prepare(plug->sample_rate);
    plug->exc_warm.prepare(plug->sample_rate);
    plug->exc_pres.prepare(plug->sample_rate);
    // Fixed voicing (drive kept modest so each narrow band stays in the CLEAN
    // shaper's pure region). Warmth: low-mids, pure even (2nd). Presence: highs,
    // even+odd (2nd+3rd). 5 narrow bands each → low intermodulation.
    plug->exc_warm.configure(/*lo*/100.0,  /*hi*/1000.0,  /*bands*/5, /*char*/0.0f, /*drive*/6.f);
    plug->exc_pres.configure(/*lo*/3500.0, /*hi*/16500.0, /*bands*/5, /*char*/0.5f, /*drive*/6.f);
    plug->exc_warm_spectrum.prepare(plug->sample_rate);
    plug->exc_pres_spectrum.prepare(plug->sample_rate);
    plug->exc_warm_db.fill(nablafx::ExciterSpectrum::kFloorDb);
    plug->exc_pres_db.fill(nablafx::ExciterSpectrum::kFloorDb);
    plug->exc_spec_active = false;
    plug->reverb.prepare(plug->sample_rate);
    plug->widener.prepare(plug->sample_rate);
    plug->ssl_viz_eq.prepare(plug->sample_rate);   // main-thread display-curve scratch
    plug->auto_gain.reset();
    plug->bc_coherence.prepare(plug->sample_rate);
    plug->meter_in.reset(plug->sample_rate);
    plug->meter_out.reset(plug->sample_rate);
    // Seed static band centres for the limiter visualization.
    plug->mel_limiter.copy_display(plug->lim_levels.data(),
                                   plug->lim_gains.data(),
                                   plug->lim_centers.data());

    plug->current_latency.store(compute_latency_(*plug), std::memory_order_relaxed);
    plug->activated = true;
    return true;
}

static void plugin_deactivate(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    plug->chains.clear();
    plug->activated = false;
}

static bool plugin_start_processing(const clap_plugin_t*) { return true; }
static void plugin_stop_processing(const clap_plugin_t*) {}

static void plugin_reset(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    plug->mel_limiter.reset();
    plug->bass_mono.reset();
    plug->exc_warm.reset();
    plug->exc_pres.reset();
    plug->exc_warm_spectrum.reset();
    plug->exc_pres_spectrum.reset();
    plug->reverb.reset();
    plug->widener.reset();
    plug->auto_gain.reset();
    plug->meter_in.reset(plug->sample_rate);
    plug->meter_out.reset(plug->sample_rate);
    for (auto& ch : plug->chains) {
        for (auto& s : ch.autoeq_ort_per_class) {
            if (s) s->reset_state();
        }
        ch.ceiling.reset(plug->sample_rate);
        ch.in_fill   = 0;
        ch.out_avail = 0;
        ch.out_read  = 0;
        ch.autoeq_peak_env = 0.f;
        // Clear the deterministic Auto-EQ controller's running spectrum + curve.
        if (!g_state->autoeq_dsp_per_class.empty()) {
            const int di = std::clamp(g_state->autoeq_default_idx, 0,
                                      (int)g_state->autoeq_dsp_per_class.size() - 1);
            ch.adaptive_eq.reset(std::get<SpectralMaskEqParams>(
                g_state->autoeq_dsp_per_class[di].params));
        }
        std::fill(ch.in_buf.begin(),  ch.in_buf.end(),  0.0f);
        std::fill(ch.out_buf.begin(), ch.out_buf.end(), 0.0f);
        if (!ch.bypass_fifo.empty())
            std::fill(ch.bypass_fifo.begin(), ch.bypass_fifo.end(), 0.0f);
    }
    plug->bypass_w = plug->bypass_r = 0;
}

// ---------------------------------------------------------------------------
// Block flush — chain one 128-sample block through every stage.
// ---------------------------------------------------------------------------

namespace {

struct AmountSnapshot {
    float autoeq_wet_mix;
    int   autoeq_cls_idx;
    float sat_pre_db, sat_post_db, sat_wet_mix, sat_hpf_hz, sat_lpf_hz, sat_thresh_lin, sat_bias;
    float trim_lin;
    float eq_range;
    bool  eq_adaptive;        // EQ_ENGINE: false = neural LSTM, true = deterministic cascade
    bool  eq_render_iir;      // EQ_RENDER: false = STFT mask, true = zero-latency IIR bank
    float eq_boost_scale;
    float eq_speed_ms;
    float ssl_comp_wet;
    float ssl_comp_in_lin;   // SSC_IN: linear input trim feeding the bus-comp model
    float ml_wet, ml_ceiling_lin, ml_drive_lin, ml_adaptive_gain, ml_adaptive_speed;
    bool  ml_adaptive_brickwall;
    float bm_wet, bm_freq;
    // HARMONICS: EXC_ON (stage on/off), EXC_WARM (low-mid 2nd level), EXC_PRES
    // (high 2nd+3rd level). Each knob 0..1 is the added-harmonic level for its
    // band; the band freqs / drive / character are fixed internally.
    bool  exc_on;
    bool  exc_solo;        // momentary: output ONLY the added harmonics (audition)
    float exc_warm, exc_pres;
    // Reverb: RVB_MIX (parallel wet blend, 0 = bypass), RVB_SIZE (room size →
    // RT60), RVB_WIDTH (tail stereo width), RVB_DAMP (tail LPF Hz), RVB_LOWCUT
    // (send high-pass Hz — bass is never reverberated).
    float rvb_mix, rvb_size, rvb_width, rvb_damp_hz, rvb_lowcut_hz;
    // Widener: WID_ON (stage on/off toggle), WID_AMT (side width gain, 1=neutral),
    // WID_FREQ (low crossover — width applies above), WID_AIR (extra HF width).
    bool  wid_on;
    float wid_amt, wid_freq, wid_air;
    bool  auto_gain_on;
    bool  bypass_on;
    // SSL 9000 J channel EQ (SEQ_*). ssl_eq_on = stage master enable; ssl_eq holds
    // the resolved biquad params built from the manual band/filter/harmonic knobs.
    bool                    ssl_eq_on;
    nablafx::SslEqParamsRT  ssl_eq;
    // Auto-EQ coupling: ssl_auto = assist amount / enable, ssl_split = α (how much
    // of the auto-EQ curve the SSL absorbs), ssl_recal = momentary recalibrate.
    float                   ssl_auto;
    float                   ssl_split;
    bool                    ssl_recal;
    bool                    ssl_reset;   // SEQ_RESET: momentary — flatten the SSL calibration
};

AmountSnapshot resolve_amount_(const Plugin& plug) {
    float sdr=0.f,svo=0.f,smx=0.5f,shf=20.f,slf=20000.f,sth=0.f,sbs=0.f;
    float mli=1.f,mlc=-1.f,mld=0.f,mlg=0.5f,mls=0.5f,mla=0.f;
    float bmi=0.f,bmf=250.f;
    float eon=0.f;                                        // harmonics on/off (switch)
    float esolo=0.f;                                      // momentary "listen" (solo wet)
    float ewarm=0.f, epres=0.f;                           // warmth / presence levels (0..1)
    float rm=0.f,rs=0.30f,rw=0.80f,rd=7000.f,rlc=250.f;  // reverb params
    float won=0.f;                                        // widener on/off (switch)
    float wa=1.f,wf=250.f,war=0.f;                        // widener params
    float agn=0.f,byp=0.f;
    float eq=0.5f,trm_db=0.f;
    float eqr=1.f,eqs=100.f,eqb=1.f;
    float eqeng=0.f;                                      // EQ_ENGINE: 0 neural, 1 adaptive
    float eqrnd=0.f;                                      // EQ_RENDER: 0 STFT mask, 1 IIR bank
    float ssc=0.f;
    float ssc_in_db=0.f;
    // SSL channel EQ (SEQ_*). Defaults = flat / filters off / no colour.
    float seon=0.f;
    float slfg=0.f,slff=100.f,slfb=0.f;
    float slmg=0.f,slmf=500.f,slmq=1.f;
    float shmg=0.f,shmf=3000.f,shmq=1.f;
    float shfg=0.f,shff=10000.f,shfb=0.f;
    float shpon=0.f,shpf=80.f,slpon=0.f,slpf=20000.f;
    float sdrv=0.f;
    float sauto=0.f,ssplit=0.6f,srecal=0.f,sreset=0.f;   // auto-EQ coupling (SEQ_AUTO/SPLIT/CAL/RESET)
    int   cls_idx=plug.active_autoeq_cls;
    for (size_t i=0;i<plug.meta->controls.size();++i) {
        const auto& c=plug.meta->controls[i];
        float v=std::clamp(plug.control_values[i],c.min,c.max);
        if(c.id=="SDR") sdr=v; else if(c.id=="SVO") svo=v;
        else if(c.id=="SMX") smx=v; else if(c.id=="SHF") shf=v; else if(c.id=="SLF") slf=v;
        else if(c.id=="STH") sth=v; else if(c.id=="SBS") sbs=v;
        else if(c.id=="EQ")  eq=v;
        else if(c.id=="CLS") cls_idx=static_cast<int>(std::lround(v));
        else if(c.id=="EQR") eqr=v; else if(c.id=="EQS") eqs=v;
        else if(c.id=="EQB") eqb=v;
        else if(c.id=="EQ_ENGINE") eqeng=v;
        else if(c.id=="EQ_RENDER") eqrnd=v;
        else if(c.id=="TRM") trm_db=v;
        else if(c.id=="SSC") ssc=v;
        else if(c.id=="SSC_IN") ssc_in_db=v;
        else if(c.id=="MLI") mli=v; else if(c.id=="MLC") mlc=v;
        else if(c.id=="MLD") mld=v;
        else if(c.id=="MLG") mlg=v; else if(c.id=="MLS") mls=v;
        else if(c.id=="MLA") mla=v;
        else if(c.id=="BMI") bmi=v; else if(c.id=="BMF") bmf=v;
        else if(c.id=="EXC_ON") eon=v;
        else if(c.id=="EXC_WARM") ewarm=v;
        else if(c.id=="EXC_PRES") epres=v;
        else if(c.id=="EXC_SOLO") esolo=v;
        else if(c.id=="RVB_MIX") rm=v; else if(c.id=="RVB_SIZE") rs=v;
        else if(c.id=="RVB_WIDTH") rw=v; else if(c.id=="RVB_DAMP") rd=v;
        else if(c.id=="RVB_LOWCUT") rlc=v;
        else if(c.id=="WID_ON") won=v;
        else if(c.id=="WID_AMT") wa=v; else if(c.id=="WID_FREQ") wf=v;
        else if(c.id=="WID_AIR") war=v;
        else if(c.id=="AGN") agn=v; else if(c.id=="BYP") byp=v;
        else if(c.id=="SEQ_ON") seon=v;
        else if(c.id=="SEQ_LF_G") slfg=v; else if(c.id=="SEQ_LF_F") slff=v; else if(c.id=="SEQ_LF_BELL") slfb=v;
        else if(c.id=="SEQ_LMF_G") slmg=v; else if(c.id=="SEQ_LMF_F") slmf=v; else if(c.id=="SEQ_LMF_Q") slmq=v;
        else if(c.id=="SEQ_HMF_G") shmg=v; else if(c.id=="SEQ_HMF_F") shmf=v; else if(c.id=="SEQ_HMF_Q") shmq=v;
        else if(c.id=="SEQ_HF_G") shfg=v; else if(c.id=="SEQ_HF_F") shff=v; else if(c.id=="SEQ_HF_BELL") shfb=v;
        else if(c.id=="SEQ_HPF_ON") shpon=v; else if(c.id=="SEQ_HPF_F") shpf=v;
        else if(c.id=="SEQ_LPF_ON") slpon=v; else if(c.id=="SEQ_LPF_F") slpf=v;
        else if(c.id=="SEQ_DRIVE") sdrv=v;
        else if(c.id=="SEQ_AUTO") sauto=v; else if(c.id=="SEQ_SPLIT") ssplit=v;
        else if(c.id=="SEQ_CAL") srecal=v;
        else if(c.id=="SEQ_RESET") sreset=v;
    }
    AmountSnapshot s{};
    s.sat_pre_db=sdr; s.sat_post_db=svo; s.sat_wet_mix=smx; s.sat_hpf_hz=shf; s.sat_lpf_hz=slf;
    s.sat_thresh_lin=std::pow(10.f, sth/20.f); s.sat_bias=sbs;
    s.autoeq_wet_mix=eq*g_state->axon_meta.amt_autoeq.wet_mix_max;
    const int n_cls = static_cast<int>(g_state->axon_meta.auto_eq.class_order.size());
    s.autoeq_cls_idx = std::clamp(cls_idx, 0, n_cls > 0 ? n_cls - 1 : 0);
    s.trim_lin=std::pow(10.f,trm_db/20.f);
    s.eq_range=eqr;
    s.eq_adaptive=(eqeng>=0.5f);
    s.eq_render_iir=(eqrnd>=0.5f);
    s.eq_boost_scale=eqb;
    s.eq_speed_ms=eqs;
    s.ssl_comp_wet = ssc * g_state->axon_meta.amt_ssl_comp.wet_mix_max;
    s.ssl_comp_in_lin = std::pow(10.f, ssc_in_db / 20.f);  // dB → linear input trim
    s.ml_wet            = mli;
    s.ml_ceiling_lin    = std::pow(10.f, mlc / 20.f);  // dBFS → linear
    s.ml_drive_lin      = std::pow(10.f, mld / 20.f);  // dB → linear gain
    s.ml_adaptive_gain  = mlg;
    s.ml_adaptive_speed = mls;
    s.ml_adaptive_brickwall = (mla >= 0.5f);
    s.bm_wet  = bmi;
    s.bm_freq = bmf;
    s.exc_on   = (eon >= 0.5f);  // stage on/off toggle
    s.exc_solo = (esolo >= 0.5f);// momentary: solo the added harmonics
    s.exc_warm = ewarm;          // 0..1 warmth (low-mid 2nd) level
    s.exc_pres = epres;          // 0..1 presence (high 2nd+3rd) level
    s.rvb_mix      = rm;     // 0..1 parallel wet blend (0 = bypass)
    s.rvb_size     = rs;     // 0..1 room size → RT60 + delay scaling
    s.rvb_width    = rw;     // 0..1 tail stereo width
    s.rvb_damp_hz  = rd;     // tail damping LPF cutoff (Hz)
    s.rvb_lowcut_hz= rlc;    // reverb send high-pass (Hz)
    s.wid_on       = (won >= 0.5f);  // stage on/off toggle
    s.wid_amt      = wa;     // side width gain (1.0 = neutral/identity)
    s.wid_freq     = wf;     // width low crossover (Hz; width applies above)
    s.wid_air      = war;    // extra high-frequency side width (above ~6 kHz)
    s.auto_gain_on = (agn >= 0.5f);
    s.bypass_on    = (byp >= 0.5f);
    // SSL channel EQ: build resolved biquad params from the manual knobs.
    s.ssl_eq_on = (seon >= 0.5f);
    {
        nablafx::SslEqParamsRT& e = s.ssl_eq;
        e.eq_on   = true;                              // bands active whenever the stage runs
        e.hpf_on  = (shpon >= 0.5f); e.hpf_hz = shpf; e.hpf_q = 0.70710678f;
        e.lpf_on  = (slpon >= 0.5f); e.lpf_hz = slpf; e.lpf_q = 0.70710678f;
        e.lf_gain = slfg; e.lf_hz = slff; e.lf_q = 0.70710678f; e.lf_bellmix = (slfb >= 0.5f) ? 1.f : 0.f;
        e.lmf_gain = slmg; e.lmf_hz = slmf; e.lmf_q = slmq;
        e.hmf_gain = shmg; e.hmf_hz = shmf; e.hmf_q = shmq;
        e.hf_gain = shfg; e.hf_hz = shff; e.hf_q = 0.70710678f; e.hf_bellmix = (shfb >= 0.5f) ? 1.f : 0.f;
        e.harmonic_mix = sdrv;                          // SEQ_DRIVE: 0 = no colour
    }
    s.ssl_auto  = sauto;
    s.ssl_split = ssplit;
    s.ssl_recal = (srecal >= 0.5f);
    s.ssl_reset = (sreset >= 0.5f);
    return s;
}

// Helpers for wet/dry blend into a buffer in-place.
static void blend_(float* buf, const float* dry, const float* wet, float w, int n) {
    for (int i=0;i<n;++i) buf[i]=(1.f-w)*dry[i]+w*wet[i];
}
static void blend_inplace_(float* buf, const float* wet, float w, int n) {
    if (w>=1.f) { std::copy_n(wet,n,buf); return; }
    for (int i=0;i<n;++i) buf[i]+=(wet[i]-buf[i])*w;
}

// Unified stage-dispatch block processor.
// Applies all user-orderable stages to work_l/work_r in plug.processor_order,
// then writes through TruePeakCeiling into each chain's out_buf.
void flush_chain_block_(Plugin& plug,
                        float* work_l, float* work_r,
                        uint32_t n_ch,
                        const AmountSnapshot& amt) {

    std::array<float,kBlockSize> dry{}, wet_a{}, wet_b{};
    // HARMONICS "Listen": when held, we capture the added-harmonics wet at the
    // Harmonics stage and substitute it for the output AFTER the chain runs, so
    // the audition isn't re-gained by the downstream limiter/ceiling.
    std::array<float,kBlockSize> solo_buf{};
    bool solo_captured = false;

    // Meter the raw plugin input (pre-chain).
    plug.meter_in.process(work_l, (n_ch >= 2 ? work_r : nullptr),
                          static_cast<int>(n_ch), kBlockSize);

    for (int pos = 0; pos < kNumStages; ++pos) {
        const int stage_idx = plug.processor_order[pos];
        switch (static_cast<StageID>(stage_idx)) {

        case StageID::SslEq: {
            // SSL 9000 J channel EQ — native biquad cascade, per channel. Zero
            // latency, coeffs recomputed at host SR inside set_params (change-
            // guarded). Master-bypassed when SEQ_ON is off (bit-identical dry).
            if (!amt.ssl_eq_on) break;
            // Auto-EQ coupling: read the main-thread-solved assist gains (seqlock —
            // retry while the generation is odd/changing), scaled by SEQ_AUTO.
            constexpr int kNA = nablafx::SslChannelEq::kNumAssist;
            std::array<float, kNA> asg{};
            if (amt.ssl_auto > 0.f) {
                for (int tries = 0; tries < 4; ++tries) {
                    uint64_t g0 = plug.ssl_asg_gen.load(std::memory_order_acquire);
                    if (g0 & 1ull) continue;                       // writer mid-update
                    asg = plug.ssl_asg_published;
                    std::atomic_thread_fence(std::memory_order_acquire);  // gains read before re-check
                    if (plug.ssl_asg_gen.load(std::memory_order_acquire) == g0) break;
                }
            }
            float* ch_buf[2] = {work_l, work_r};
            for (uint32_t ch = 0; ch < n_ch; ++ch) {
                auto& e = plug.chains[ch].ssl_eq;
                e.set_params(amt.ssl_eq);
                // Ramp the assist gains toward the published target (~2.7 s one-pole
                // per 128-sample block) so the static solve engages smoothly.
                auto& sm = plug.chains[ch].ssl_asg_smooth;
                for (int b = 0; b < kNA; ++b)
                    sm[b] = 0.999f * sm[b] + 0.001f * (amt.ssl_auto * asg[b]);
                e.set_assist_gains(sm.data(), kNA);
                e.process(ch_buf[ch], nullptr, kBlockSize);   // in place, this channel's state
            }
            break;
        }

        case StageID::AutoEQ: {
            // If CLS changed since the previous block, swap the active class
            // and zero out the new session's LSTM state so we don't carry over
            // hidden activations conditioned on a different signal class.
            if (amt.autoeq_cls_idx != plug.active_autoeq_cls) {
                plug.active_autoeq_cls = amt.autoeq_cls_idx;
                for (auto& chan : plug.chains) {
                    auto& s = chan.autoeq_ort_per_class[plug.active_autoeq_cls];
                    if (s) s->reset_state();
                    chan.autoeq_peak_env = 0.f;
                }
            }
            const int cls = plug.active_autoeq_cls;
            const auto& cls_dsp = g_state->autoeq_dsp_per_class[cls];
            const int   n_params =
                std::get<SpectralMaskEqParams>(cls_dsp.params).num_control_params;
            // Adaptive target curve follows the CLS selector: map the active
            // class name (bass/drums/vocals/other/full_mix) to its empirical
            // tonal-balance curve. Unknown → full_mix (set_target_curve clamps).
            const auto& aeq_classes = g_state->axon_meta.auto_eq.class_order;
            const int adaptive_curve_idx =
                (cls >= 0 && cls < static_cast<int>(aeq_classes.size()))
                    ? nablafx::adaptive_eq_detail::target_curve_index(aeq_classes[cls].c_str())
                    : 0;
            std::array<float, 64> eq_params_storage{};
            float* eq_params = eq_params_storage.data();
            float* ch_buf[2] = {work_l, work_r};
            for (uint32_t ch=0; ch<n_ch; ++ch) {
                float* blk = ch_buf[ch];
                // EQ_ENGINE: fill eq_params from either the deterministic cascade
                // controller (Adaptive) or the per-class LSTM (Neural). Both emit
                // the same [0,1] band contract; the renderer below is unchanged.
                if (amt.eq_adaptive) {
                    // Range is applied by the renderer's set_range_norm below, so
                    // ask the controller for the full-depth curve (depth01 = 1).
                    auto& actrl = plug.chains[ch].adaptive_eq;
                    actrl.set_target_curve(adaptive_curve_idx);   // follow CLS
                    actrl.set_response_ms(amt.eq_speed_ms);       // EQ Speed knob
                    actrl.observe(blk, kBlockSize);
                    actrl.target_bands(eq_params, n_params, 1.0f);
                } else {
                    auto& sess = plug.chains[ch].autoeq_ort_per_class[cls];
                    // Peak-hold envelope normalisation to match training distribution.
                    std::array<float, kBlockSize> ctrl_buf;
                    float blk_peak = 0.f;
                    for (int i = 0; i < kBlockSize; ++i)
                        blk_peak = std::max(blk_peak, std::abs(blk[i]));
                    auto& env = plug.chains[ch].autoeq_peak_env;
                    if (blk_peak > env) env = blk_peak;
                    else env = plug.autoeq_env_decay * env
                              + (1.f - plug.autoeq_env_decay) * blk_peak;
                    const float ctrl_scale = (env > 1e-6f) ? (0.5f / env) : 1.f;
                    for (int i = 0; i < kBlockSize; ++i) ctrl_buf[i] = blk[i] * ctrl_scale;
                    sess->run_controller(ctrl_buf.data(), kBlockSize, eq_params, n_params);
                    sess->swap_state();
                }

                std::copy_n(blk, kBlockSize, dry.data());
                // Display frequencies: 5-point overlay at the historical PEQ band
                // centres + 50-point log-spaced bin curve (20–20k Hz).
                static constexpr float kDisplayHz[5] =
                    {1010.f, 110.f, 1100.f, 7000.f, 10000.f};
                static const std::array<float, SpectrumAnalyzer::kNumBins> kBinHz = []() {
                    std::array<float, SpectrumAnalyzer::kNumBins> hz;
                    for (int i = 0; i < SpectrumAnalyzer::kNumBins; ++i)
                        hz[i] = 20.f * std::pow(1000.f,
                            float(i) / (SpectrumAnalyzer::kNumBins - 1));
                    return hz;
                }();
                // EQ_RENDER: zero-latency minimum-phase IIR bank vs the STFT mask.
                // Both consume the same eq_params [0,1] contract.
                if (amt.eq_render_iir) {
                    auto& dsp = plug.chains[ch].autoeq_iir_per_class[cls];
                    dsp->set_range_norm(amt.eq_range);   // IIR bank: range only
                    dsp->set_params(eq_params, n_params);
                    dsp->process(blk, wet_a.data(), kBlockSize);
                    if (ch == 0) {
                        float gains5[5];
                        for (int k = 0; k < 5; ++k)
                            gains5[k] = static_cast<float>(dsp->magnitude_db(kDisplayHz[k]));
                        plug.spectrum.set_eq_gains(gains5);
                        float gains50[SpectrumAnalyzer::kNumBins];
                        for (int k = 0; k < SpectrumAnalyzer::kNumBins; ++k)
                            gains50[k] = static_cast<float>(dsp->magnitude_db(kBinHz[k]));
                        plug.spectrum.set_eq_bins(gains50);
                    }
                } else {
                    // Spectral mask: range scales the predicted dB curve toward
                    // 0 dB; speed sets the bin-gain smoother time constant. Both
                    // applied inside set_params on each tick.
                    auto& dsp = plug.chains[ch].autoeq_spec_per_class[cls];
                    dsp->set_range_norm(amt.eq_range);
                    dsp->set_boost_scale(amt.eq_boost_scale);
                    dsp->set_speed_tau_ms(amt.eq_speed_ms);
                    dsp->set_params(eq_params, n_params);
                    dsp->process(blk, wet_a.data(), kBlockSize);
                    if (ch == 0) {
                        float gains5[5];
                        dsp->sample_gains_db(kDisplayHz, gains5, 5);
                        plug.spectrum.set_eq_gains(gains5);
                        float gains50[SpectrumAnalyzer::kNumBins];
                        dsp->sample_gains_db(kBinHz.data(), gains50,
                                             SpectrumAnalyzer::kNumBins);
                        plug.spectrum.set_eq_bins(gains50);
                    }
                }
                blend_(blk, dry.data(), wet_a.data(), amt.autoeq_wet_mix, kBlockSize);
            }
            break;
        }

        case StageID::Saturator: {
            const float pre  = std::pow(10.f, amt.sat_pre_db / 20.f);
            const float pst  = std::pow(10.f, amt.sat_post_db / 20.f);
            const float T    = amt.sat_thresh_lin;
            const float invT = 1.f / T;
            const bool  use_hpf = amt.sat_hpf_hz > 21.f;
            const bool  use_lpf = amt.sat_lpf_hz < 19000.f;
            float* ch_buf[2] = {work_l, work_r};
            for (uint32_t ch = 0; ch < n_ch; ++ch) {
                float* blk   = ch_buf[ch];
                auto&  chain = plug.chains[ch];
                // DC offset introduced by bias; subtracted after eval to keep output AC.
                const float dc = static_cast<float>(
                    chain.saturator.eval(static_cast<double>(amt.sat_bias)));
                if (use_hpf || use_lpf) {
                    if (use_hpf && std::abs(amt.sat_hpf_hz - chain.sat_hpf_fc) > 0.5f) {
                        const float K = std::tan(
                            static_cast<float>(M_PI) * amt.sat_hpf_hz
                            / static_cast<float>(plug.sample_rate));
                        const float norm = 1.f / (1.f + K);
                        chain.sat_hpf_b0 =  norm;
                        chain.sat_hpf_b1 = -norm;
                        chain.sat_hpf_a1 = (K - 1.f) * norm;
                        chain.sat_hpf_fc = amt.sat_hpf_hz;
                    }
                    if (use_lpf && std::abs(amt.sat_lpf_hz - chain.sat_lpf_fc) > 0.5f) {
                        const float K = std::tan(
                            static_cast<float>(M_PI) * amt.sat_lpf_hz
                            / static_cast<float>(plug.sample_rate));
                        const float norm = 1.f / (1.f + K);
                        chain.sat_lpf_b0 = K * norm;
                        chain.sat_lpf_b1 = K * norm;
                        chain.sat_lpf_a1 = (K - 1.f) * norm;
                        chain.sat_lpf_fc = amt.sat_lpf_hz;
                    }
                    // Apply HPF then optional LPF to extract the saturation band.
                    // wet_a = band; bypass = blk - band; recombined below.
                    for (int i = 0; i < kBlockSize; ++i) {
                        float s = blk[i];
                        if (use_hpf) {
                            const float x = s;
                            s = chain.sat_hpf_b0 * x
                              + chain.sat_hpf_b1 * chain.sat_hpf_x1
                              - chain.sat_hpf_a1 * chain.sat_hpf_y1;
                            chain.sat_hpf_x1 = x;
                            chain.sat_hpf_y1 = s;
                        }
                        if (use_lpf) {
                            const float x = s;
                            s = chain.sat_lpf_b0 * x
                              + chain.sat_lpf_b1 * chain.sat_lpf_x1
                              - chain.sat_lpf_a1 * chain.sat_lpf_y1;
                            chain.sat_lpf_x1 = x;
                            chain.sat_lpf_y1 = s;
                        }
                        wet_a[i] = s;
                    }
                    for (int i = 0; i < kBlockSize; ++i) {
                        const float x_in = wet_a[i] * pre * invT + amt.sat_bias;
                        wet_b[i] = (static_cast<float>(chain.saturator.eval(
                            static_cast<double>(x_in))) - dc) * T * pst;
                    }
                    for (int i = 0; i < kBlockSize; ++i)
                        blk[i] = (blk[i] - wet_a[i])
                                + (1.f - amt.sat_wet_mix) * wet_a[i]
                                +         amt.sat_wet_mix  * wet_b[i];
                } else {
                    std::copy_n(blk, kBlockSize, dry.data());
                    for (int i = 0; i < kBlockSize; ++i) {
                        const float x_in = blk[i] * pre * invT + amt.sat_bias;
                        wet_a[i] = (static_cast<float>(chain.saturator.eval(
                            static_cast<double>(x_in))) - dc) * T * pst;
                    }
                    blend_(blk, dry.data(), wet_a.data(), amt.sat_wet_mix, kBlockSize);
                }
            }
            break;
        }

        case StageID::SslComp: {
            // SSL-style bus comp: stateless long-RF causal TCN with hop
            // accumulation. The wet output queue trails the input by
            // (kSslHop - kBlockSize) samples — we delay the dry signal by the
            // same amount via a per-channel ring so the wet/dry blend stays
            // sample-aligned (otherwise blending current dry with delayed
            // wet produces a hop-rate comb-filter flutter).
            //
            // Streaming-mode TCN export contract: the ONNX takes `trace_len`
            // samples in (the entire ring, including the `rf-1` history
            // prefix) and produces `trace_len - (rf-1)` samples out — the
            // model's predictions for ring positions [rf-1, trace_len-1].
            // Output position i corresponds to ring position (rf-1 + i).
            //
            // Skipped if the SSL bundle wasn't shipped (ssl_comp_ort null) or
            // the wet mix is at zero.
            if (amt.ssl_comp_wet <= 0.f || !plug.chains[0].ssl_comp_ort) {
                // Stage idle/bypassed: park the meter at idle and reset the
                // coherence averages so re-enabling starts clean.
                plug.bc_distortion_db = nablafx::CoherenceDistortion::floor_db();
                plug.bc_crest_red_db  = 0.f;
                plug.bc_active        = false;
                plug.bc_coherence.reset();
                break;
            }
            const int N           = g_state->ssl_comp_meta.trace_len;
            const int rf          = g_state->ssl_comp_meta.receptive_field;
            const int actual_olen = N - (rf - 1);  // ORT output length
            const int dry_delay_len = kSslHop - kBlockSize;
            // Input trim → model operating point. The bus comp is a fixed-curve
            // model whose behaviour depends only on how hard it's driven; we
            // attenuate/boost the signal feeding the model by `in_gain` and
            // undo it with the exact reciprocal `mk_gain` on the wet output, so
            // a static trim is level-neutral while changing how hard the comp
            // works. The dry/bypass path gets NEITHER gain (it stays the clean
            // reference). At 0 dB both gains are 1.0 → bit-for-bit unchanged.
            // NOTE: the model output trails the input by hop/lookahead, so the
            // input gain and the reciprocal make-up are applied at slightly
            // different points in time. For a static trim this is exactly
            // correct; fast automation of the trim can cause a brief level
            // bump — acceptable for an operating-point control.
            const float in_gain = amt.ssl_comp_in_lin;
            const float mk_gain = (in_gain != 0.f) ? (1.f / in_gain) : 1.f;
            // "Crunch" telemetry accumulators. The crest-reduction proxy is a
            // running peak + sum-of-squares over the block (unchanged). The
            // DISTORTION proxy is phase-invariant coherence, fed the MONO SUM
            // 0.5*(L+R) of the time-aligned dry and the post-makeup wet — built
            // here per sample position across the channel loop, then pushed to
            // bc_coherence once after the loop. Read-only: does NOT touch the
            // blend, makeup, or alignment.
            double crunch_dry2 = 0.0, crunch_wet2 = 0.0;
            float  crunch_peak_dry = 0.f, crunch_peak_wet = 0.f;
            int    crunch_n = 0;   // samples accumulated (0 → still warming up)
            std::array<float, kBlockSize> mono_dry{}, mono_wet{};
            bool   coh_have_block = false;   // a full wet block was popped
            float* ch_buf[2]={work_l,work_r};
            for (uint32_t ch=0;ch<n_ch;++ch) {
                float* blk=ch_buf[ch];
                std::copy_n(blk,kBlockSize,dry.data());

                auto& accum  = plug.chains[ch].ssl_comp_in_accum;
                auto& ring   = plug.chains[ch].ssl_comp_in_ring;
                auto& obuf   = plug.chains[ch].ssl_comp_out_buf;
                auto& outq   = plug.chains[ch].ssl_comp_out_queue;
                auto& dryd   = plug.chains[ch].ssl_comp_dry_delay;
                int&  fill   = plug.chains[ch].ssl_comp_in_fill;
                int&  avail  = plug.chains[ch].ssl_comp_out_avail;
                int&  rd     = plug.chains[ch].ssl_comp_out_read;
                int&  dwr    = plug.chains[ch].ssl_comp_dry_write;

                // Push input into the hop-sized accumulator, applying the input
                // trim so the MODEL sees the attenuated/boosted signal. `dry`
                // was captured above and is untouched, so the dry/bypass and
                // warm-up pass-through paths remain the clean reference.
                if (in_gain != 1.f) {
                    for (int i = 0; i < kBlockSize; ++i)
                        accum[fill + i] = blk[i] * in_gain;
                } else {
                    std::copy_n(blk, kBlockSize, accum.data() + fill);
                }
                fill += kBlockSize;

                // When the accumulator fills, shift the ring by kSslHop, append
                // the accumulator at the tail, run ORT once, and stash the
                // trailing kSslHop samples of output into the playback queue.
                if (fill >= kSslHop) {
                    std::memmove(ring.data(),
                                 ring.data() + kSslHop,
                                 (N - kSslHop) * sizeof(float));
                    std::copy_n(accum.data(), kSslHop,
                                ring.data() + N - kSslHop);
                    fill = 0;

                    // ORT produces `actual_olen` (= N - rf + 1) samples;
                    // pass that as audio_out_len so OrtMiniSession doesn't
                    // read past the tensor end.
                    plug.chains[ch].ssl_comp_ort->run(ring.data(), N,
                        obuf.data(), actual_olen,
                        nullptr, 0, "audio_out");

                    // Output sample i corresponds to ring position (rf-1+i).
                    // We want the kSslHop newest predictions (ring positions
                    // [N-kSslHop, N-1]), which are at output positions
                    // [N-kSslHop-(rf-1), actual_olen-1] = the LAST kSslHop
                    // samples of the actual output.
                    std::copy_n(obuf.data() + actual_olen - kSslHop, kSslHop,
                                outq.data());
                    avail = kSslHop;
                    rd    = 0;
                }

                // Build a per-sample time-aligned dry block. With dry delay
                // length = kSslHop - kBlockSize, the dry sample we need to
                // blend against this call's wet was written into the delay
                // (kSslHop / kBlockSize - 1) host calls ago. Read those
                // samples out, then write the new dry in for future use.
                std::array<float, kBlockSize> dry_aligned{};
                if (dry_delay_len > 0) {
                    const int len = static_cast<int>(dryd.size());
                    for (int i = 0; i < kBlockSize; ++i) {
                        const int read_idx = (dwr + i) % len;
                        dry_aligned[i] = dryd[read_idx];
                        dryd[read_idx] = dry[i];   // overwrite with new dry
                    }
                    dwr = (dwr + kBlockSize) % len;
                } else {
                    std::copy_n(dry.data(), kBlockSize, dry_aligned.data());
                }

                // Pop kBlockSize samples from the output queue. While the
                // queue is short of a full block (the kSslHop-sample warm-up
                // window after activate, before the first ORT call), leave
                // the current dry untouched in blk so the host hears the
                // bypassed signal instead of silence or a comb of stale-zero
                // delayed-dry against silence.
                if (avail >= kBlockSize) {
                    std::copy_n(outq.data() + rd, kBlockSize, wet_a.data());
                    rd    += kBlockSize;
                    avail -= kBlockSize;
                    // Reciprocal make-up: undo the input trim on the wet output
                    // so the stage stays roughly level-neutral. Dry is left
                    // alone, so only the comp's *character* changes with trim.
                    if (mk_gain != 1.f) {
                        for (int i = 0; i < kBlockSize; ++i)
                            wet_a[i] *= mk_gain;
                    }
                    // "Crunch" tap: read the post-makeup, pre-blend wet_a and
                    // the time-aligned dry. Read-only; does NOT touch the blend,
                    // makeup, or alignment. Accumulate the mono sum 0.5*(L+R)
                    // per sample for coherence, plus sum-of-squares + peaks for
                    // the crest-reduction proxy.
                    const float mono_w = (n_ch > 1) ? 0.5f : 1.f;
                    for (int i = 0; i < kBlockSize; ++i) {
                        const float d = dry_aligned[i];
                        const float w = wet_a[i];
                        mono_dry[i] += mono_w * d;
                        mono_wet[i] += mono_w * w;
                        crunch_dry2 += static_cast<double>(d) * d;
                        crunch_wet2 += static_cast<double>(w) * w;
                        const float ad = std::fabs(d), aw = std::fabs(w);
                        if (ad > crunch_peak_dry) crunch_peak_dry = ad;
                        if (aw > crunch_peak_wet) crunch_peak_wet = aw;
                    }
                    crunch_n += kBlockSize;
                    coh_have_block = true;
                    // Blend the (delayed) wet against the time-aligned dry,
                    // NOT the current dry — they're the same audio in
                    // absolute time, so this is the only mix that doesn't
                    // produce hop-rate comb-filter flutter.
                    blend_inplace_(wet_a.data(), dry_aligned.data(),
                                   1.f - amt.ssl_comp_wet, kBlockSize);
                    std::copy_n(wet_a.data(), kBlockSize, blk);
                }
                // else: leave blk = current dry (warm-up pass-through).
            }

            // Reduce the accumulated sums into the two "crunch" proxy metrics.
            // During the warm-up window (no full output block yet) crunch_n is
            // 0 → hold idle so the strip doesn't flash garbage.
            if (crunch_n > 0) {
                // Feed the mono dry/wet block to the coherence analyzer; it runs
                // its own FFT framing/EMA internally and gates on silence/warmup.
                if (coh_have_block)
                    plug.bc_coherence.push(mono_dry.data(), mono_wet.data(), kBlockSize);
                plug.bc_distortion_db = plug.bc_coherence.distortion_db();

                const double inv = 1.0 / static_cast<double>(crunch_n);
                const float rms_dry = static_cast<float>(std::sqrt(crunch_dry2 * inv));
                const float rms_wet = static_cast<float>(std::sqrt(crunch_wet2 * inv));
                constexpr float eps = 1e-9f;
                // Crest reduction = crest(dry) - crest(wet); positive == squashed.
                const float crest_dry = 20.f * std::log10((crunch_peak_dry + eps) / (rms_dry + eps));
                const float crest_wet = 20.f * std::log10((crunch_peak_wet + eps) / (rms_wet + eps));
                float crest_red = crest_dry - crest_wet;
                if (crest_red < 0.f)  crest_red = 0.f;
                plug.bc_crest_red_db = crest_red;
                plug.bc_active       = true;
            } else {
                plug.bc_distortion_db = nablafx::CoherenceDistortion::floor_db();
                plug.bc_crest_red_db  = 0.f;
                plug.bc_active        = false;
            }
            break;
        }

        case StageID::MelLimiter: {
            if (amt.ml_wet <= 0.f) break;
            nablafx::MelLimiter::Params mlp;
            mlp.ceiling_lin    = amt.ml_ceiling_lin;
            mlp.drive_lin      = amt.ml_drive_lin;
            mlp.adaptive_gain  = amt.ml_adaptive_gain;
            mlp.adaptive_speed = amt.ml_adaptive_speed;
            mlp.wet_mix        = amt.ml_wet;
            mlp.adaptive_brickwall = amt.ml_adaptive_brickwall;
            plug.mel_limiter.process(work_l, work_r, static_cast<int>(n_ch),
                                     kBlockSize, mlp);
            break;
        }

        case StageID::BassMono: {
            if (amt.bm_wet <= 0.f || n_ch < 2) break;
            plug.bass_mono.set_cutoff(amt.bm_freq);
            // Process a copy so bm_wet can dial the effect in/out.
            std::array<float,kBlockSize> bl{}, br{};
            std::copy_n(work_l, kBlockSize, bl.data());
            std::copy_n(work_r, kBlockSize, br.data());
            plug.bass_mono.process(bl.data(), br.data(), kBlockSize);
            blend_inplace_(work_l, bl.data(), amt.bm_wet, kBlockSize);
            blend_inplace_(work_r, br.data(), amt.bm_wet, kBlockSize);
            break;
        }

        case StageID::Exciter: {
            // HARMONICS — two CLEAN (polynomial) parallel exciters in series.
            // Warmth excites the low-mids (100 Hz–1 kHz, pure even/2nd → body);
            // Presence excites the highs (3.5 kHz–16.5 kHz, even+odd → air). Each
            // knob is the added-harmonic LEVEL for its band (drive/band/character
            // fixed); both at 0 → bit-identical bypass.
            if (!amt.exc_on || (amt.exc_warm <= 0.f && amt.exc_pres <= 0.f)) {
                plug.exc_spec_active = false;
                plug.exc_warm_spectrum.reset();
                plug.exc_pres_spectrum.reset();
                break;
            }
            float* wr = (n_ch >= 2 ? work_r : nullptr);
            // Voicing (band range / character / drive) is fixed in activate via
            // configure(); the knob is just the added-harmonic LEVEL.
            plug.exc_warm.set_amount(amt.exc_warm);
            plug.exc_warm.process(work_l, wr, kBlockSize, wet_a.data());
            plug.exc_pres.set_amount(amt.exc_pres);
            plug.exc_pres.process(work_l, wr, kBlockSize, wet_b.data());
            // "Listen" (momentary): capture ONLY the added harmonics (warm +
            // presence wet). The chain keeps running normally (so stage state
            // stays continuous); the capture is substituted for the output after
            // the loop, below — so the audition isn't re-gained by the limiter.
            if (amt.exc_solo) {
                for (int i = 0; i < kBlockSize; ++i) solo_buf[i] = wet_a[i] + wet_b[i];
                solo_captured = true;
            }
            // Per-band wet-spectrum overlays (mono wet contribution of each band).
            plug.exc_warm_spectrum.push(wet_a.data(), kBlockSize);
            plug.exc_pres_spectrum.push(wet_b.data(), kBlockSize);
            plug.exc_warm_spectrum.copy_spectrum(plug.exc_warm_db.data());
            plug.exc_pres_spectrum.copy_spectrum(plug.exc_pres_db.data());
            plug.exc_spec_active = true;
            break;
        }

        case StageID::Reverb: {
            // Transparent mastering room reverb (8-line FDN). The dry path is
            // passed at unity and untouched; the module sums in `mix` of the
            // decorrelated wet tail. At mix == 0 it early-returns (bit-identical
            // bypass) and adds ZERO reported latency (the dry is never delayed).
            if (amt.rvb_mix <= 0.f) break;
            plug.reverb.set_params(amt.rvb_mix, amt.rvb_size, amt.rvb_width,
                                   amt.rvb_damp_hz, amt.rvb_lowcut_hz);
            plug.reverb.process(work_l, (n_ch >= 2 ? work_r : nullptr),
                                kBlockSize);
            break;
        }

        case StageID::Widener: {
            // Transparent M/S stereo widener. Frequency-dependent side gain — the
            // mono sum (L+R = 2·M) is preserved exactly, so this is mono-compatible
            // by construction and adds ZERO latency. Gated by the WID_ON toggle;
            // also a no-op at width == 1 && air == 0 (bit-identical bypass), and a
            // no-op on mono (n_ch < 2 → no side to widen).
            if (!amt.wid_on || n_ch < 2) break;
            if (amt.wid_amt == 1.f && amt.wid_air == 0.f) break;
            plug.widener.set_params(amt.wid_amt, amt.wid_freq, amt.wid_air);
            plug.widener.process(work_l, work_r, kBlockSize);
            break;
        }

        } // switch

        // Capture stage output for the spectrum analyzer.
        plug.spectrum.push(pos, work_l, work_r, n_ch, kBlockSize);
    } // for stage

    // When a full 2048-sample frame has accumulated, hand off to the main thread.
    if (plug.spectrum.advance_and_transfer())
        plug.host->request_callback(plug.host);

    // HARMONICS "Listen": substitute the captured added-harmonics for the output
    // (mono → both channels), bypassing the downstream chain's gain so the user
    // hears exactly what's being added at its true level. Still passes through the
    // ceiling below as a safety cap (harmonics are quiet → it won't engage).
    if (solo_captured) {
        std::copy_n(solo_buf.data(), kBlockSize, work_l);
        if (n_ch >= 2) std::copy_n(solo_buf.data(), kBlockSize, work_r);
    }

    // Trim + TruePeakCeiling — always last, not user-reorderable. This is the
    // REAL master; the OUT meter reads it (so driving the limiter shows the
    // actual target loudness), and Auto Gain is applied *after* metering.
    if (amt.trim_lin != 1.f) {
        for (int i=0;i<kBlockSize;++i) work_l[i]*=amt.trim_lin;
        if (n_ch>=2) for (int i=0;i<kBlockSize;++i) work_r[i]*=amt.trim_lin;
    }
    for (uint32_t ch=0;ch<n_ch;++ch) {
        float* blk=(ch==0)?work_l:work_r;
        plug.chains[ch].ceiling.process(blk,plug.chains[ch].out_buf.data(),kBlockSize);
        plug.chains[ch].out_avail=kBlockSize;
        plug.chains[ch].out_read=0;
    }

    // Meter the real master (pre Auto Gain) and publish both meters.
    plug.meter_out.process(plug.chains[0].out_buf.data(),
                           (n_ch >= 2 ? plug.chains[1].out_buf.data() : nullptr),
                           static_cast<int>(n_ch), kBlockSize);
    {
        const auto in_r  = plug.meter_in.readout();
        const auto out_r = plug.meter_out.readout();
        plug.m_in_lufs_s.store(in_r.lufs_s,  std::memory_order_relaxed);
        plug.m_in_lufs_m.store(in_r.lufs_m,  std::memory_order_relaxed);
        plug.m_in_rms.store(in_r.rms_db,     std::memory_order_relaxed);
        plug.m_in_peak.store(in_r.peak_db,   std::memory_order_relaxed);
        plug.m_out_lufs_s.store(out_r.lufs_s, std::memory_order_relaxed);
        plug.m_out_lufs_m.store(out_r.lufs_m, std::memory_order_relaxed);
        plug.m_out_rms.store(out_r.rms_db,    std::memory_order_relaxed);
        plug.m_out_peak.store(out_r.peak_db,  std::memory_order_relaxed);
    }

    // Auto gain (level-matched bypass): a *monitoring* trim that brings the
    // delivered output down to the input's loudness for fair A/B. Computed
    // feed-forward from the real (metered) output, applied after metering — so
    // the OUT meter keeps showing the true master level. It's monitoring-only:
    // render with Auto Gain off to print the loud master.
    // Feed-forward the limiter Drive (a known gain, scaled by limiter wet) so a
    // Drive change is cancelled instantly instead of bumping for ~3 s while the
    // LUFS loop catches up.
    const float drive_db = 20.f * std::log10(std::max(1e-6f, amt.ml_drive_lin));
    const float ff_db    = drive_db * amt.ml_wet;
    float ag;
    if (amt.exc_solo) {
        // HARMONICS "Listen" is an audition: the output drops to the (quiet) wet,
        // so we FREEZE Auto Gain (hold its current makeup) instead of letting it
        // chase the audition — otherwise it slowly swells up during the press and
        // leaves the makeup cranked on release (a loud spike when the mix returns).
        ag = std::pow(10.f, plug.auto_gain.gain_db() / 20.f);
    } else {
        ag = plug.auto_gain.process(amt.auto_gain_on,
                                    plug.meter_in.readout().lufs_s,
                                    plug.meter_out.readout().lufs_s,
                                    ff_db);
    }
    if (ag != 1.f) {
        for (uint32_t ch=0;ch<n_ch;++ch) {
            float* ob = plug.chains[ch].out_buf.data();
            for (int i=0;i<kBlockSize;++i) ob[i] *= ag;
        }
    }
}

// Index of a control by id in the loaded meta (main thread; -1 if absent).
static int control_index_(const Plugin& plug, const char* id) {
    for (size_t i = 0; i < plug.meta->controls.size(); ++i)
        if (plug.meta->controls[i].id == id) return (int)i;
    return -1;
}

// Push a param value onto the GUI->audio queue (thread-safe; the audio thread drains
// it into control_values) AND reflect it on the WebView knob. Used by the coupling +
// reset to write the visible SSL gain knobs from the main thread.
static void set_visible_param_(Plugin& plug, const char* id, float value) {
    const int idx = control_index_(plug, id);
    if (idx < 0) return;
    { std::lock_guard<std::mutex> lk(plug.param_mutex); plug.param_queue.emplace_back(idx, value); }
    if (plug.gui_state) {
        char js[128];
        std::snprintf(js, sizeof(js), "axonSetParam(\"%s\",%.4f);", id, value);
        axon_gui_eval_js(plug.gui_state, js);
    }
}

// Auto-EQ -> SSL coupling ("calibration"). On a SEQ_CAL rising edge, fit the FOUR
// real SSL bands (the user's LF/LMF/HMF/HF at their current freq/Q/shelf-or-bell) to
// SEQ_SPLIT * the current auto-EQ curve (spectrum.mt_eq_bins), then ADD the solved
// gains onto the VISIBLE LF/LMF/HMF/HF knobs. This keeps the correction musical (a
// real broad SSL curve, not 6 narrow bells) and visible. It is a single partial step
// per press — it deliberately does NOT auto-contract to full cancellation (the auto-
// EQ is transparent and should keep doing the residual); press again to absorb more.
// The hidden 6-band assist layer (ssl_asg_*) is left dormant for now.
static void solve_ssl_coupling_(Plugin& plug, bool /*spec_ready*/) {
    const AmountSnapshot amt = resolve_amount_(plug);

    // Reset (SEQ_RESET rising edge) — flatten the 4 visible SSL gain knobs and clear
    // the dormant assist layer. Runs regardless of the SEQ_AUTO gate.
    const bool reset_edge = amt.ssl_reset && !plug.ssl_reset_prev;
    plug.ssl_reset_prev = amt.ssl_reset;
    if (reset_edge) {
        static const char* kId[4] = { "SEQ_LF_G", "SEQ_LMF_G", "SEQ_HMF_G", "SEQ_HF_G" };
        for (int b = 0; b < 4; ++b) set_visible_param_(plug, kId[b], 0.f);
        plug.ssl_asg_accum.fill(0.f);
        const uint64_t gen = plug.ssl_asg_gen.load(std::memory_order_relaxed);
        plug.ssl_asg_gen.store(gen + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        plug.ssl_asg_published.fill(0.f);
        plug.ssl_asg_gen.store(gen + 2, std::memory_order_release);
        for (auto& ch : plug.chains) ch.ssl_asg_smooth.fill(0.f);
        plug.ssl_solve_timer = 0;
    }

    if (amt.ssl_auto <= 0.f || !amt.ssl_eq_on) { plug.ssl_recal_prev = amt.ssl_recal; return; }
    const bool recal_edge = amt.ssl_recal && !plug.ssl_recal_prev;
    plug.ssl_recal_prev = amt.ssl_recal;
    if (!recal_edge) return;                                  // CAL-triggered only

    constexpr int NB = SpectrumAnalyzer::kNumBins;
    double f[NB], tgt[NB];
    for (int k = 0; k < NB; ++k) {
        f[k]   = 20.0 * std::pow(1000.0, (double)k / (NB - 1));   // 20 Hz-20 kHz log
        tgt[k] = amt.ssl_split * (double)plug.spectrum.mt_eq_bins[k];
    }

    // Solver bands = the 4 real SSL bands at their CURRENT freq/Q/type (LF/HF follow
    // the shelf<->bell switch). type: 0 bell, 1 lo-shelf, 2 hi-shelf.
    const nablafx::SslEqParamsRT& e = amt.ssl_eq;
    std::vector<nablafx::SslSolverBand> bands = {
        { e.lf_bellmix >= 0.5f ? 0 : 1, (double)e.lf_hz,  (double)e.lf_q  },
        { 0,                            (double)e.lmf_hz, (double)e.lmf_q },
        { 0,                            (double)e.hmf_hz, (double)e.hmf_q },
        { e.hf_bellmix >= 0.5f ? 0 : 2, (double)e.hf_hz,  (double)e.hf_q  },
    };
    nablafx::SslEqSolver solver(std::move(bands));
    const std::vector<double> dg = solver.solve(f, tgt, NB, plug.sample_rate, 9.0);
    if ((int)dg.size() < 4) return;

    // Accumulate onto the visible gain knobs (the knobs ARE the integrator), clamped
    // to the SEQ_*_G range, then push through the GUI->audio path + GUI display.
    const float cur[4]        = { e.lf_gain, e.lmf_gain, e.hmf_gain, e.hf_gain };
    static const char* kId[4] = { "SEQ_LF_G", "SEQ_LMF_G", "SEQ_HMF_G", "SEQ_HF_G" };
    for (int b = 0; b < 4; ++b)
        set_visible_param_(plug, kId[b], std::clamp(cur[b] + (float)dg[b], -18.f, 18.f));
}

}  // namespace

// ---------------------------------------------------------------------------
// process — accumulator-driven 128-sample block flushes
// ---------------------------------------------------------------------------

static void apply_events_(Plugin* plug, const clap_input_events_t* in_events) {
    if (!in_events) return;
    const uint32_t n = in_events->size(in_events);
    for (uint32_t i = 0; i < n; ++i) {
        const auto* hdr = in_events->get(in_events, i);
        if (!hdr) continue;
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (hdr->type != CLAP_EVENT_PARAM_VALUE) continue;
        const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(hdr);
        for (size_t k = 0; k < plug->meta->controls.size(); ++k) {
            if (param_id_for(plug->meta->effect_name, plug->meta->controls[k].id) == pv->param_id) {
                plug->control_values[k] = static_cast<float>(pv->value);
                break;
            }
        }
    }
}

// GUI-thread callbacks — write pending changes into thread-safe queues.
static void axon_on_param_change(void* plug_ptr, const char* param_id, float value) {
    auto* plug = static_cast<Plugin*>(plug_ptr);
    for (size_t i = 0; i < plug->meta->controls.size(); ++i) {
        if (plug->meta->controls[i].id == param_id) {
            std::lock_guard<std::mutex> lk(plug->param_mutex);
            plug->param_queue.emplace_back(static_cast<int>(i), value);
            break;
        }
    }
}
static void axon_on_order_change(void* plug_ptr, const int* order, int count) {
    auto* plug = static_cast<Plugin*>(plug_ptr);
    if (count != kNumStages) return;
    std::lock_guard<std::mutex> lk(plug->order_mutex);
    for (int i = 0; i < kNumStages; ++i) plug->pending_order[i] = order[i];
    plug->order_pending = true;
}

static clap_process_status plugin_process(const clap_plugin_t* p, const clap_process_t* process) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    apply_events_(plug, process->in_events);

    // Drain GUI param queue (try_lock — never blocks audio thread).
    {
        std::unique_lock<std::mutex> lk(plug->param_mutex, std::try_to_lock);
        if (lk.owns_lock() && !plug->param_queue.empty()) {
            for (auto& [idx, val] : plug->param_queue)
                plug->control_values[idx] = val;
            plug->param_queue.clear();
        }
    }
    // Drain GUI order change.
    {
        std::unique_lock<std::mutex> lk(plug->order_mutex, std::try_to_lock);
        if (lk.owns_lock() && plug->order_pending) {
            plug->processor_order = plug->pending_order;
            plug->order_pending = false;
        }
    }

    // Recompute latency after any param updates; notify host on change.
    {
        const uint32_t new_lat = compute_latency_(*plug);
        if (new_lat != plug->current_latency.load(std::memory_order_relaxed)) {
            plug->current_latency.store(new_lat, std::memory_order_relaxed);
            plug->latency_needs_notify.store(true, std::memory_order_relaxed);
            plug->host->request_callback(plug->host);
        }
    }

    const uint32_t n_frames = process->frames_count;
    if (n_frames == 0) return CLAP_PROCESS_CONTINUE;
    if (process->audio_inputs_count == 0 || process->audio_outputs_count == 0)
        return CLAP_PROCESS_ERROR;

    const float* const* in_ch  = process->audio_inputs[0].data32;
    float* const*       out_ch = process->audio_outputs[0].data32;
    const uint32_t n_ch = std::min<uint32_t>(
        static_cast<uint32_t>(plug->chains.size()),
        std::min(process->audio_inputs[0].channel_count,
                 process->audio_outputs[0].channel_count));
    if (n_ch == 0) return CLAP_PROCESS_ERROR;

    AmountSnapshot amt = resolve_amount_(*plug);

    // All channels fill/drain at the same rate, so use one shared in_pos/out_pos.
    uint32_t in_pos = 0, out_pos = 0;

    constexpr int kBR = Plugin::kBypassRing;
    while (out_pos < n_frames) {
        // Drain all channels' output rings together. The dry FIFO is popped in
        // lock-step (always, to stay aligned); Bypass selects it over the wet.
        while (out_pos < n_frames && plug->chains[0].out_read < plug->chains[0].out_avail) {
            const int rd = plug->bypass_r % kBR;
            for (uint32_t ch = 0; ch < n_ch; ++ch) {
                const float wet = plug->chains[ch].out_buf[plug->chains[ch].out_read];
                out_ch[ch][out_pos] = amt.bypass_on ? plug->chains[ch].bypass_fifo[rd] : wet;
            }
            for (uint32_t ch = 0; ch < n_ch; ++ch)
                ++plug->chains[ch].out_read;
            ++plug->bypass_r;
            ++out_pos;
        }
        if (out_pos >= n_frames) break;

        if (in_pos >= n_frames) {
            while (out_pos < n_frames) {
                for (uint32_t ch = 0; ch < n_ch; ++ch) out_ch[ch][out_pos] = 0.0f;
                ++out_pos;
            }
            break;
        }

        // Push input into all channels' accumulators simultaneously.
        const uint32_t take = std::min<uint32_t>(
            n_frames - in_pos,
            static_cast<uint32_t>(kBlockSize - plug->chains[0].in_fill));
        for (uint32_t ch = 0; ch < n_ch; ++ch) {
            std::copy_n(in_ch[ch] + in_pos,
                        take,
                        plug->chains[ch].in_buf.data() + plug->chains[ch].in_fill);
            plug->chains[ch].in_fill += take;
            // Mirror the raw input into the Bypass FIFO.
            float* fifo = plug->chains[ch].bypass_fifo.data();
            for (uint32_t i = 0; i < take; ++i)
                fifo[(plug->bypass_w + i) % kBR] = in_ch[ch][in_pos + i];
        }
        plug->bypass_w += take;
        in_pos += take;

        if (plug->chains[0].in_fill < kBlockSize) {
            // Accumulator not yet full — pad output with zeros.
            while (out_pos < n_frames) {
                for (uint32_t ch = 0; ch < n_ch; ++ch) out_ch[ch][out_pos] = 0.0f;
                ++out_pos;
            }
            break;
        }

        // Blocks are full — run the unified stage chain.
        std::array<float, kBlockSize> work_l{}, work_r{};
        std::copy_n(plug->chains[0].in_buf.data(), kBlockSize, work_l.data());
        if (n_ch >= 2) std::copy_n(plug->chains[1].in_buf.data(), kBlockSize, work_r.data());
        flush_chain_block_(*plug, work_l.data(), work_r.data(), n_ch, amt);
        for (uint32_t ch = 0; ch < n_ch; ++ch)
            plug->chains[ch].in_fill = 0;
    }

    // Snapshot limiter band state for the UI (try_lock — never blocks audio).
    if (plug->lim_mtx.try_lock()) {
        plug->mel_limiter.copy_display(plug->lim_levels.data(),
                                       plug->lim_gains.data(),
                                       plug->lim_centers.data());
        plug->lim_ceiling = amt.ml_ceiling_lin;
        plug->lim_brick   = plug->mel_limiter.brickwall_gain();
        plug->lim_active  = (amt.ml_wet > 0.f);
        plug->lim_mtx.unlock();
    }
    return CLAP_PROCESS_CONTINUE;
}

// ---------------------------------------------------------------------------
// GUI extension  (CLAP_EXT_GUI, CLAP_WINDOW_API_COCOA)
// ---------------------------------------------------------------------------

static void gui_send_full_state_(Plugin* plug) {
    if (!plug->gui_state) return;
    const size_t n = plug->meta->controls.size();
    std::vector<AxonParamInfo> params(n);
    // Cache class-name pointers for the CLS enum picker. The vector itself
    // backs the const char* array we pass in AxonParamInfo::enum_options;
    // both must outlive the axon_gui_send_init() call (it copies the strings
    // into the JS payload synchronously on the main thread or buffers them).
    const auto& classes = g_state->axon_meta.auto_eq.class_order;
    std::vector<const char*> class_ptrs;
    class_ptrs.reserve(classes.size());
    for (const auto& s : classes) class_ptrs.push_back(s.c_str());

    for (size_t i = 0; i < n; ++i) {
        const auto& c = plug->meta->controls[i];
        params[i].id             = c.id.c_str();
        params[i].name           = c.name.c_str();
        params[i].min            = c.min;
        params[i].max            = c.max;
        params[i].def            = c.def;
        params[i].unit           = c.unit.c_str();
        params[i].current_value  = plug->control_values[i];
        params[i].enum_options   = nullptr;
        params[i].n_enum_options = 0;
        if (c.id == "CLS" && !class_ptrs.empty()) {
            params[i].enum_options   = class_ptrs.data();
            params[i].n_enum_options = static_cast<int>(class_ptrs.size());
        }
    }
    axon_gui_send_init(plug->gui_state,
                       params.data(), static_cast<int>(n),
                       plug->processor_order.data(), kNumStages);
}

static bool gui_is_api_supported(const clap_plugin_t*, const char* api, bool is_floating) {
    return !is_floating && std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
}
static bool gui_get_preferred_api(const clap_plugin_t*, const char** api, bool* is_floating) {
    *api = CLAP_WINDOW_API_COCOA;
    *is_floating = false;
    return true;
}
static bool gui_create(const clap_plugin_t* p, const char* api, bool is_floating) {
    if (!gui_is_api_supported(p, api, is_floating)) return false;
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    if (plug->gui_state) return true;  // already exists
    plug->gui_state = axon_gui_create(
        plug,
        g_state->resources_dir.c_str(),
        axon_on_param_change,
        axon_on_order_change);
    return plug->gui_state != nullptr;
}
static void gui_destroy_fn(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    axon_gui_destroy(plug->gui_state);
    plug->gui_state = nullptr;
}
static bool gui_set_scale(const clap_plugin_t*, double) { return false; }
static bool gui_get_size(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    axon_gui_get_size(w, h);
    return true;
}
static bool gui_can_resize(const clap_plugin_t*) { return false; }
static bool gui_get_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    hints->can_resize_horizontally = false;
    hints->can_resize_vertically   = false;
    hints->preserve_aspect_ratio   = false;
    hints->aspect_ratio_width  = 820;
    hints->aspect_ratio_height = 560;
    return false;
}
static bool gui_adjust_size(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    axon_gui_get_size(w, h);
    return true;
}
static bool gui_set_size(const clap_plugin_t*, uint32_t, uint32_t) { return true; }
static bool gui_set_parent(const clap_plugin_t* p, const clap_window_t* window) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    if (!plug->gui_state) return false;
    return axon_gui_set_parent(plug->gui_state, window->cocoa);
}
static bool gui_set_transient(const clap_plugin_t*, const clap_window_t*) { return false; }
static void gui_suggest_title(const clap_plugin_t*, const char*) {}
static bool gui_show(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    if (!plug->gui_state) return false;
    gui_send_full_state_(plug);
    axon_gui_show(plug->gui_state);
    return true;
}
static bool gui_hide(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);
    axon_gui_hide(plug->gui_state);
    return true;
}

static const clap_plugin_gui_t s_ext_gui = {
    gui_is_api_supported,
    gui_get_preferred_api,
    gui_create,
    gui_destroy_fn,
    gui_set_scale,
    gui_get_size,
    gui_can_resize,
    gui_get_resize_hints,
    gui_adjust_size,
    gui_set_size,
    gui_set_parent,
    gui_set_transient,
    gui_suggest_title,
    gui_show,
    gui_hide,
};

static const void* plugin_get_extension(const clap_plugin_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &s_ext_audio_ports;
    if (std::strcmp(id, CLAP_EXT_PARAMS)       == 0) return &s_ext_params;
    if (std::strcmp(id, CLAP_EXT_LATENCY)      == 0) return &s_ext_latency;
    if (std::strcmp(id, CLAP_EXT_STATE)        == 0) return &s_ext_state;
    if (std::strcmp(id, CLAP_EXT_GUI)          == 0) return &s_ext_gui;
    return nullptr;
}

static void plugin_on_main_thread(const clap_plugin_t* p) {
    auto* plug = static_cast<Plugin*>(p->plugin_data);

    if (plug->latency_needs_notify.exchange(false, std::memory_order_relaxed)) {
        if (plug->host_latency_ext && plug->host_latency_ext->changed)
            plug->host_latency_ext->changed(plug->host);
    }

    // Pump the spectrum transport regardless of GUI so the SSL coupling solve runs
    // headless (the assist-band fit needs a fresh auto-EQ curve every ~1 s).
    const bool spec_ready = plug->spectrum.process_if_ready(plug->sample_rate);
    solve_ssl_coupling_(*plug, spec_ready);

    if (!plug->gui_state) return;
    if (spec_ready) {
        const std::string js = plug->spectrum.build_js(plug->processor_order);
        axon_gui_eval_js(plug->gui_state, js.c_str());
    }

    // SSL EQ display curve: the stage's TOTAL magnitude response — manual bands
    // PLUS the published coupling assist gains (scaled by SEQ_AUTO) — at the same
    // 50 log bins as the auto-EQ curve. This is what lets the UI show what the SSL
    // contributes incl. the assist bands, which is what makes SEQ_CAL visible.
    {
        const AmountSnapshot amt = resolve_amount_(*plug);
        auto& e = plug->ssl_viz_eq;
        e.set_params(amt.ssl_eq);
        constexpr int NA = nablafx::SslChannelEq::kNumAssist;
        std::array<float, NA> asg{};
        if (amt.ssl_auto > 0.f) {
            for (int tries = 0; tries < 4; ++tries) {
                uint64_t g0 = plug->ssl_asg_gen.load(std::memory_order_acquire);
                if (g0 & 1ull) continue;
                asg = plug->ssl_asg_published;
                std::atomic_thread_fence(std::memory_order_acquire);  // gains read before re-check
                if (plug->ssl_asg_gen.load(std::memory_order_acquire) == g0) break;
            }
            for (int b = 0; b < NA; ++b) asg[b] *= amt.ssl_auto;
        }
        e.set_assist_gains(asg.data(), NA);
        constexpr int NB = SpectrumAnalyzer::kNumBins;
        std::string js = "axonSslCurve({\"on\":";
        js += amt.ssl_eq_on ? "true" : "false";
        js += ",\"bins\":[";
        char buf[16];
        for (int k = 0; k < NB; ++k) {
            if (k) js += ',';
            const double hz = 20.0 * std::pow(1000.0, (double)k / (NB - 1));
            std::snprintf(buf, sizeof(buf), "%.2f", e.magnitude_db(hz));
            js += buf;
        }
        js += "]});";
        axon_gui_eval_js(plug->gui_state, js.c_str());
    }

    // Push in/out meter readings (same ~21 fps cadence as the spectrum).
    {
        char mjs[320];
        std::snprintf(mjs, sizeof(mjs),
            "axonMeters({\"in\":{\"lufs_s\":%.1f,\"lufs_m\":%.1f,\"rms\":%.1f,\"peak\":%.1f},"
            "\"out\":{\"lufs_s\":%.1f,\"lufs_m\":%.1f,\"rms\":%.1f,\"peak\":%.1f}})",
            plug->m_in_lufs_s.load(std::memory_order_relaxed),
            plug->m_in_lufs_m.load(std::memory_order_relaxed),
            plug->m_in_rms.load(std::memory_order_relaxed),
            plug->m_in_peak.load(std::memory_order_relaxed),
            plug->m_out_lufs_s.load(std::memory_order_relaxed),
            plug->m_out_lufs_m.load(std::memory_order_relaxed),
            plug->m_out_rms.load(std::memory_order_relaxed),
            plug->m_out_peak.load(std::memory_order_relaxed));
        axon_gui_eval_js(plug->gui_state, mjs);
    }

    // Push limiter band visualization (levels + gain reduction per Mel band).
    {
        constexpr int NB = nablafx::MelLimiter::num_bands();
        std::array<float, NB> f{}, lvl{}, gr{};
        float ceil_lin, brick_lin; bool active;
        {
            std::lock_guard<std::mutex> lk(plug->lim_mtx);
            f = plug->lim_centers; ceil_lin = plug->lim_ceiling;
            brick_lin = plug->lim_brick; active = plug->lim_active;
            for (int b = 0; b < NB; ++b) {
                const float L = plug->lim_levels[b];
                const float G = plug->lim_gains[b];
                lvl[b] = (L > 1e-6f) ? 20.f * std::log10(L) : -90.f;   // band level dB
                gr[b]  = (G > 1e-6f) ? 20.f * std::log10(G) : -60.f;   // gain reduction dB (≤0)
            }
        }
        const float ceil_db  = (ceil_lin  > 1e-6f) ? 20.f * std::log10(ceil_lin)  : -90.f;
        const float brick_db = (brick_lin > 1e-6f) ? 20.f * std::log10(brick_lin) : -24.f; // peak GR ≤0
        std::string js;
        js.reserve(1024);
        js = "axonLimiter({\"active\":";
        js += (active ? "true" : "false");
        char nb[24];
        js += ",\"brick\":";
        std::snprintf(nb, sizeof(nb), "%.1f", brick_db); js += nb;
        js += ",\"ceiling\":";
        std::snprintf(nb, sizeof(nb), "%.1f", ceil_db); js += nb;
        auto arr = [&](const char* key, const std::array<float,NB>& a, int dec) {
            js += ",\""; js += key; js += "\":[";
            for (int b = 0; b < NB; ++b) {
                if (b) js += ',';
                std::snprintf(nb, sizeof(nb), dec == 0 ? "%.0f" : "%.1f", a[b]);
                js += nb;
            }
            js += ']';
        };
        arr("f", f, 0); arr("lvl", lvl, 1); arr("gr", gr, 1);
        js += "})";
        axon_gui_eval_js(plug->gui_state, js.c_str());
    }

    // Push Bus Comp "crunch" telemetry (model-activity proxy). Plain member
    // reads — a benign meter race, same pattern as the limiter brick gain.
    {
        char bjs[160];
        std::snprintf(bjs, sizeof(bjs),
            "axonBusComp({\"active\":%s,\"distortion\":%.1f,\"crest\":%.1f})",
            plug->bc_active ? "true" : "false",
            plug->bc_distortion_db,
            plug->bc_crest_red_db);
        axon_gui_eval_js(plug->gui_state, bjs);
    }

    // Push the HARMONICS two-zone overlay: per-band wet-spectrum dBFS magnitudes
    // ("warm" = low-mid 2nd, "pres" = high 2nd+3rd), each kNumBins log-spaced at
    //   f_i = 20 * (20000/20)^(i/(kNumBins-1)) Hz  (20 Hz .. 20 kHz inclusive),
    // which the UI reconstructs with the identical log formula. Empty/floored
    // bins draw nothing, so each zone's glow is exactly what that knob adds.
    {
        constexpr int NEB = nablafx::ExciterSpectrum::kNumBins;
        std::string ejs;
        ejs.reserve(96 + NEB * 14);
        ejs = "axonExciter({\"active\":";
        ejs += (plug->exc_spec_active ? "true" : "false");
        char eb[16];
        auto append_arr = [&](const char* key, const float* db) {
            ejs += key;
            for (int i = 0; i < NEB; ++i) {
                if (i) ejs += ',';
                std::snprintf(eb, sizeof(eb), "%.1f", db[i]);
                ejs += eb;
            }
            ejs += "]";
        };
        append_arr(",\"warm\":[", plug->exc_warm_db.data());
        append_arr(",\"pres\":[", plug->exc_pres_db.data());
        ejs += "})";
        axon_gui_eval_js(plug->gui_state, ejs.c_str());
    }
}

// ---------------------------------------------------------------------------
// Factory + entry
// ---------------------------------------------------------------------------

static const clap_plugin_t* factory_create_plugin(const clap_plugin_factory_t*,
                                                  const clap_host_t* host,
                                                  const char* plugin_id) {
    if (!g_state) return nullptr;
    if (std::strcmp(plugin_id, g_state->plugin_id_str.c_str()) != 0) return nullptr;

    auto* plug = new Plugin{};
    plug->host     = host;
    plug->meta     = &g_state->axon_meta;
    plug->channels = 2;

    plug->plugin.desc             = &g_state->descriptor;
    plug->plugin.plugin_data      = plug;
    plug->plugin.init             = plugin_init;
    plug->plugin.destroy          = plugin_destroy;
    plug->plugin.activate         = plugin_activate;
    plug->plugin.deactivate       = plugin_deactivate;
    plug->plugin.start_processing = plugin_start_processing;
    plug->plugin.stop_processing  = plugin_stop_processing;
    plug->plugin.reset            = plugin_reset;
    plug->plugin.process          = plugin_process;
    plug->plugin.get_extension    = plugin_get_extension;
    plug->plugin.on_main_thread   = plugin_on_main_thread;
    return &plug->plugin;
}

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t*) { return g_state ? 1 : 0; }
static const clap_plugin_descriptor_t* factory_get_plugin_descriptor(const clap_plugin_factory_t*,
                                                                     uint32_t index) {
    if (!g_state || index != 0) return nullptr;
    return &g_state->descriptor;
}

static const clap_plugin_factory_t s_factory = {
    factory_get_plugin_count, factory_get_plugin_descriptor, factory_create_plugin,
};

static bool entry_init(const char* /*plugin_path*/) {
    if (g_state) return true;
    try {
        auto st = std::make_unique<ModuleState>();
        st->bundle_dir   = find_bundle_contents_();
        if (st->bundle_dir.empty()) return false;
        st->resources_dir = st->bundle_dir + "/Resources";

        st->axon_meta = load_composite_meta(st->resources_dir + "/axon_meta.json");

        // SSL bus comp input trim ("Input"). The bus comp is a neural model of
        // a FIXED-curve compressor — its character is set entirely by how hard
        // the input drives it. This dB control attenuates/boosts the signal
        // feeding the model so the user can land on the model's sweet spot
        // without inserting a gain plugin ahead of Axon. Injected at load time
        // (rather than required from the bundle) so existing bundles that don't
        // declare it still get the control with a safe default. Skip injection
        // if a bundle already declares "SSC_IN" so its ranges win.
        {
            bool has_ssc_in = false;
            for (const auto& c : st->axon_meta.controls)
                if (c.id == "SSC_IN") { has_ssc_in = true; break; }
            if (!has_ssc_in) {
                st->axon_meta.controls.push_back(ControlSpec{
                    "SSC_IN", "Input", -24.0f, 12.0f, 0.0f, 1.0f, "dB"});
            }
        }

        // Aphex-style exciter controls. Injected at load time (like SSC_IN) so
        // existing bundles that don't declare them still get the stage with safe
        // defaults; a bundle that declares an id keeps its own ranges. Defaults
        // make the stage a no-op (EXC_AMT = 0 → bit-identical bypass).
        {
            auto inject = [&](const ControlSpec& spec) {
                for (const auto& c : st->axon_meta.controls)
                    if (c.id == spec.id) return;        // bundle's ranges win
                st->axon_meta.controls.push_back(spec);
            };
            // HARMONICS: two-knob clean exciter (Warmth = low-mid 2nd, Presence =
            // high 2nd+3rd). Defaults make the stage a no-op (both knobs 0 →
            // bit-identical bypass).
            //                  id            name         min      max     def    skew unit
            inject(ControlSpec{"EXC_ON",    "Harmonics",   0.0f,    1.0f,   1.0f,  1.0f, "switch"});
            inject(ControlSpec{"EXC_WARM",  "Warmth",      0.0f,    1.0f,   0.0f,  1.0f, ""});
            inject(ControlSpec{"EXC_PRES",  "Presence",    0.0f,    1.0f,   0.0f,  1.0f, ""});
            inject(ControlSpec{"EXC_SOLO",  "Listen",      0.0f,    1.0f,   0.0f,  1.0f, "switch"});
            // Transparent mastering room reverb. Defaults make the stage a no-op
            // (RVB_MIX = 0 → bit-identical bypass) so existing bundles that don't
            // declare these still get the controls with safe defaults.
            inject(ControlSpec{"RVB_MIX",   "Mix",       0.0f,    1.0f,    1.0f, 1.0f, ""});
            inject(ControlSpec{"RVB_SIZE",  "Size",      0.0f,    1.0f,    0.30f,1.0f, ""});
            inject(ControlSpec{"RVB_WIDTH", "Width",     0.0f,    1.0f,    1.0f,1.0f, ""});
            inject(ControlSpec{"RVB_DAMP",  "Damp",   2000.0f,18000.0f, 7000.0f, 1.0f, "Hz"});
            inject(ControlSpec{"RVB_LOWCUT","Low Cut",  20.0f, 1000.0f,  250.0f, 1.0f, "Hz"});
            // Transparent M/S stereo widener. Defaults make the stage a no-op
            // (WID_ON = OFF; and width == 1 / air == 0 → bit-identical bypass) so
            // existing bundles that don't declare these still get safe controls.
            inject(ControlSpec{"WID_ON",    "Width",    0.0f,    1.0f,    1.0f, 1.0f, "switch"});
            inject(ControlSpec{"WID_AMT",   "Amount",   0.0f,    2.0f,   1.38f, 1.0f, ""});
            inject(ControlSpec{"WID_FREQ",  "Low",     50.0f, 1000.0f,  250.0f, 1.0f, "Hz"});
            inject(ControlSpec{"WID_AIR",   "Air",      0.0f,    1.0f,    1.0f, 1.0f, ""});
            // SSL 9000 J channel EQ (SEQ_*). Defaults make the stage a no-op
            // (SEQ_ON = OFF → bit-identical bypass). LF/HF switch shelf<->bell via
            // *_BELL; LMF/HMF carry Q; HPF/LPF have on/off + cutoff. SEQ_AUTO/SPLIT/CAL
            // are the Auto-EQ coupling (assist bands absorb the Auto-EQ correction).
            inject(ControlSpec{"SEQ_ON",     "SSL EQ",       0.0f,     1.0f,    0.0f,  1.0f, "switch"});
            inject(ControlSpec{"SEQ_LF_G",   "LF Gain",    -18.0f,    18.0f,    0.0f,  1.0f, "dB"});
            inject(ControlSpec{"SEQ_LF_F",   "LF Freq",     30.0f,   600.0f,  100.0f,  1.0f, "Hz"});
            inject(ControlSpec{"SEQ_LF_BELL","LF Bell",      0.0f,     1.0f,    0.0f,  1.0f, "switch"});
            inject(ControlSpec{"SEQ_LMF_G",  "LMF Gain",   -18.0f,    18.0f,    0.0f,  1.0f, "dB"});
            inject(ControlSpec{"SEQ_LMF_F",  "LMF Freq",    60.0f,  3000.0f,  500.0f,  1.0f, "Hz"});
            inject(ControlSpec{"SEQ_LMF_Q",  "LMF Q",        0.1f,     4.0f,    1.0f,  1.0f, ""});
            inject(ControlSpec{"SEQ_HMF_G",  "HMF Gain",   -18.0f,    18.0f,    0.0f,  1.0f, "dB"});
            inject(ControlSpec{"SEQ_HMF_F",  "HMF Freq",   400.0f, 20000.0f, 3000.0f,  1.0f, "Hz"});
            inject(ControlSpec{"SEQ_HMF_Q",  "HMF Q",        0.1f,     4.0f,    1.0f,  1.0f, ""});
            inject(ControlSpec{"SEQ_HF_G",   "HF Gain",    -18.0f,    18.0f,    0.0f,  1.0f, "dB"});
            inject(ControlSpec{"SEQ_HF_F",   "HF Freq",   1500.0f, 20000.0f,10000.0f,  1.0f, "Hz"});
            inject(ControlSpec{"SEQ_HF_BELL","HF Bell",      0.0f,     1.0f,    0.0f,  1.0f, "switch"});
            inject(ControlSpec{"SEQ_HPF_ON", "HPF",          0.0f,     1.0f,    0.0f,  1.0f, "switch"});
            inject(ControlSpec{"SEQ_HPF_F",  "HPF Freq",    20.0f,   500.0f,   80.0f,  1.0f, "Hz"});
            inject(ControlSpec{"SEQ_LPF_ON", "LPF",          0.0f,     1.0f,    0.0f,  1.0f, "switch"});
            inject(ControlSpec{"SEQ_LPF_F",  "LPF Freq",  3000.0f, 22000.0f,20000.0f,  1.0f, "Hz"});
            inject(ControlSpec{"SEQ_DRIVE",  "Colour",       0.0f,     1.0f,    0.0f,  1.0f, ""});
            inject(ControlSpec{"SEQ_AUTO",   "Auto Assist",  0.0f,     1.0f,    0.0f,  1.0f, ""});
            inject(ControlSpec{"SEQ_SPLIT",  "Split",        0.0f,     1.0f,    0.6f,  1.0f, ""});
            inject(ControlSpec{"SEQ_CAL",    "Recalibrate",  0.0f,     1.0f,    0.0f,  1.0f, "switch"});
            inject(ControlSpec{"SEQ_RESET",  "Reset",        0.0f,     1.0f,    0.0f,  1.0f, "switch"});
        }

        st->sat_meta    = load_meta(st->resources_dir + "/" +
                                    st->axon_meta.sub_bundles.at("saturator")
                                    + "/plugin_meta.json");
        // SSL bus comp is optional — older bundles don't ship it. If present,
        // load its meta so we can size per-channel ring buffers at activate.
        if (st->axon_meta.sub_bundles.count("ssl_comp")) {
            st->ssl_comp_meta = load_meta(
                st->resources_dir + "/" + st->axon_meta.sub_bundles.at("ssl_comp")
                + "/plugin_meta.json");
            st->ssl_comp_loaded = true;
        }

        // Load every auto-EQ class meta in the canonical class_order. All
        // classes must declare spectral_mask_eq with identical geometry
        // (n_fft, hop, n_bands, gain range, frequency range), since the
        // runtime SpectralMaskEq downstream is shared and only the
        // controller ONNX is swapped on a class change.
        st->autoeq_metas.clear();
        st->autoeq_class_index.clear();
        st->autoeq_metas.reserve(st->axon_meta.auto_eq.class_order.size());
        for (const auto& cls : st->axon_meta.auto_eq.class_order) {
            const std::string& dir = st->axon_meta.auto_eq.classes.at(cls);
            PluginMeta m = load_meta(st->resources_dir + "/" + dir + "/plugin_meta.json");
            if (m.dsp_blocks.empty()) {
                throw std::runtime_error(
                    "auto_eq sub-bundle '" + dir + "' has no dsp_blocks");
            }
            st->autoeq_class_index[cls] = st->autoeq_metas.size();
            st->autoeq_metas.push_back(std::move(m));
        }
        if (st->autoeq_metas.empty()) {
            throw std::runtime_error("axon_meta: auto_eq is empty");
        }

        // Pull the DSP block payloads we need at chain construction time.
        if (st->sat_meta.dsp_blocks.empty()) {
            throw std::runtime_error("saturator sub-bundle has no dsp_blocks");
        }
        st->sat_rational = std::get<RationalAParams>(st->sat_meta.dsp_blocks[0].params);
        // Per-class DSP-block payloads (all spectral_mask_eq).
        st->autoeq_dsp_per_class.clear();
        st->autoeq_dsp_per_class.reserve(st->autoeq_metas.size());
        for (const auto& m : st->autoeq_metas) {
            st->autoeq_dsp_per_class.push_back(m.dsp_blocks[0]);
        }
        st->autoeq_default_idx = static_cast<int>(
            st->autoeq_class_index.at(st->axon_meta.auto_eq.default_class));

        st->ort_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "nablafx-tone");
        populate_descriptor_(*st);
        g_state = st.release();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

static void entry_deinit() { delete g_state; g_state = nullptr; }

static const void* entry_get_factory(const char* factory_id) {
    if (std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &s_factory;
    return nullptr;
}

}  // namespace nablafx_axon

extern "C" {
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    nablafx_axon::entry_init,
    nablafx_axon::entry_deinit,
    nablafx_axon::entry_get_factory,
};
}
