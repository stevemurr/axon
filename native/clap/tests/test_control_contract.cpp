// Guards the meta<->C++ control contract.
//
// Regression target: `axon/export/composite.py` has drifted from the C++ more
// than once, so the shipped `weights/axon_bundle/axon_meta.json` control set can
// silently disagree with what `axon_plugin.cpp` actually reads (`resolve_amount_`
// et al., the `c.id == "..."` compares). Two failure modes:
//   - C++ reads a control the meta lacks  -> that stage stalls at its default.
//   - meta declares a control C++ ignores -> a dead automation knob.
//
// This test extracts BOTH sets and asserts they are identical, and separately
// asserts the 57 SEQ_* controls (global + three independent EQ banks) are present.
//
// NOTE the extractor contract: axon_plugin.cpp must spell every control read
// as a literal `c.id == "..."` compare (see the comment above resolve_amount_
// there). Reads in any other form are invisible to this test.
//
// Paths are injected by CMake (SRC_PATH = axon_plugin.cpp, META_PATH = the
// committed axon_meta.json) so the test runs against the real, shipped artifacts.

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

static std::string slurp(const char* path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "FATAL: cannot open %s\n", path); std::exit(2); }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Every `c.id == "XXX"` string-literal compare in the plugin source = the set of
// controls the C++ actually consumes.
static std::set<std::string> cpp_read_set(const std::string& src) {
    std::set<std::string> ids;
    const std::regex re(R"(c\.id\s*==\s*\"([A-Z0-9_]+)\")");
    for (std::sregex_iterator it(src.begin(), src.end(), re), end; it != end; ++it)
        ids.insert((*it)[1].str());
    return ids;
}

static std::set<std::string> meta_control_set(const nlohmann::json& meta) {
    std::set<std::string> ids;
    for (auto& [key, spec] : meta.at("controls").items()) ids.insert(key);
    return ids;
}

static void print_diff(const char* label, const std::set<std::string>& a,
                       const std::set<std::string>& b) {
    for (const auto& x : a) if (!b.count(x)) std::fprintf(stderr, "  %s: %s\n", label, x.c_str());
}

int main() {
    const std::string src = slurp(SRC_PATH);
    nlohmann::json meta = nlohmann::json::parse(slurp(META_PATH));

    const auto cpp  = cpp_read_set(src);
    const auto mset = meta_control_set(meta);

    std::fprintf(stderr, "[contract] C++ read-set = %zu, meta controls = %zu\n",
                 cpp.size(), mset.size());

    // 1) No control the C++ reads is missing from the meta.
    std::set<std::string> missing;
    for (const auto& x : cpp) if (!mset.count(x)) missing.insert(x);
    if (!missing.empty()) { std::fprintf(stderr, "C++ reads but meta LACKS:\n"); print_diff("missing", cpp, mset); }
    assert(missing.empty() && "meta is missing controls the plugin reads (stage would stall at default)");

    // 2) No control in the meta is ignored by the C++ (dead knob).
    std::set<std::string> dead;
    for (const auto& x : mset) if (!cpp.count(x)) dead.insert(x);
    if (!dead.empty()) { std::fprintf(stderr, "meta declares but C++ IGNORES:\n"); print_diff("dead", mset, cpp); }
    assert(dead.empty() && "meta declares dead knobs the plugin never reads");

    std::fprintf(stderr, "[contract] meta == C++ read-set (no missing, no dead) PASS\n");

    // 3) Global EQ controls + 17 manual controls in each Stereo/Mid/Side bank.
    std::set<std::string> seq_expected = {
        "SEQ_ON", "SEQ_MODE", "SEQ_AUTO", "SEQ_SPLIT", "SEQ_CAL", "SEQ_RESET"
    };
    static const char* kPrefixes[] = {"SEQ_", "SEQ_MID_", "SEQ_SIDE_"};
    static const char* kSuffixes[] = {
        "LF_G","LF_F","LF_BELL","LMF_G","LMF_F","LMF_Q",
        "HMF_G","HMF_F","HMF_Q","HF_G","HF_F","HF_BELL",
        "HPF_ON","HPF_F","LPF_ON","LPF_F","DRIVE"
    };
    for (const char* prefix : kPrefixes)
        for (const char* suffix : kSuffixes)
            seq_expected.insert(std::string(prefix) + suffix);
    assert(seq_expected.size() == 57 && "EQ bank contract count drifted");
    const auto& controls = meta.at("controls");
    for (const auto& id : seq_expected) {
        assert(controls.contains(id) && "SSL EQ control missing from meta");
        const auto& c = controls.at(id);
        const double mn = c.at("min"), mx = c.at("max"), df = c.at("default");
        assert(mn <= mx && "control min > max");
        assert(df >= mn && df <= mx && "control default out of [min,max]");
    }
    // SEQ_ON defaults ON (EQ engaged out of the box, per product default). With
    // flat bands / HPF-LPF off / drive 0 the stage is near-transparent — no longer
    // bit-identical bypass, but musically neutral until the user dials the EQ.
    assert(double(controls.at("SEQ_ON").at("default")) == 1.0 && "SEQ_ON must default on");
    assert(double(controls.at("SEQ_MODE").at("default")) == 0.0 && "SEQ_MODE must default to Stereo");
    assert(double(controls.at("SEQ_MODE").at("max")) == 2.0 && "SEQ_MODE must expose Stereo/Mid/Side");
    // SEQ_SPLIT (coupling alpha) default = 0.6.
    assert(double(controls.at("SEQ_SPLIT").at("default")) == 0.6 && "SEQ_SPLIT default drifted");
    std::fprintf(stderr, "[contract] 57 SEQ_* controls (3 banks) present + specs sane PASS\n");

    std::fprintf(stderr, "ALL CONTROL-CONTRACT TESTS PASSED\n");
    return 0;
}
