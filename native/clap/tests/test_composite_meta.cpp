// Standalone sanity test for the composite axon_meta.json loader. Runs on
// Linux because composite_meta.cpp has no CLAP / ORT deps.
//
// Build:
//   g++ -O2 -std=c++17 -I../src -I<json>/single_include \
//       test_composite_meta.cpp ../src/composite_meta.cpp ../src/meta.cpp \
//       -o test_composite_meta
//
// Run (point at the staged composite dir):
//   ./test_composite_meta /tmp/axon-staging/axon_meta.json

#include "../src/composite_meta.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1]
                                   : std::getenv("AXON_META_JSON");
#ifdef DEFAULT_META_PATH
    // CMake injects the committed weights/axon_bundle/axon_meta.json so the
    // canonical suite runs this against the shipped artifact with no args.
    if (!path) path = DEFAULT_META_PATH;
#endif
    if (!path) {
        std::fprintf(stderr,
            "usage: test_composite_meta <axon_meta.json>  (or set AXON_META_JSON)\n");
        return 2;
    }
    auto m = nablafx::load_composite_meta(path);

    std::fprintf(stderr, "schema_version: %d\n",   m.schema_version);
    std::fprintf(stderr, "effect_name:    %s\n",   m.effect_name.c_str());
    std::fprintf(stderr, "model_id:       %s\n",   m.model_id.c_str());
    std::fprintf(stderr, "sample_rate:    %d\n",   m.sample_rate);
    std::fprintf(stderr, "channels:       %d\n",   m.channels);
    std::fprintf(stderr, "sub_bundles:\n");
    for (const auto& [k, v] : m.sub_bundles) {
        std::fprintf(stderr, "  %s -> %s\n", k.c_str(), v.c_str());
    }
    std::fprintf(stderr, "controls:\n");
    for (const auto& c : m.controls) {
        std::fprintf(stderr, "  %s (%s) [%g..%g] def=%g unit=%s\n",
                     c.id.c_str(), c.name.c_str(), c.min, c.max, c.def, c.unit.c_str());
    }
    std::fprintf(stderr, "ssl_comp: wet_mix_max=%g\n", m.amt_ssl_comp.wet_mix_max);
    std::fprintf(stderr, "autoeq:  wet_mix_max=%g\n", m.amt_autoeq.wet_mix_max);
    std::fprintf(stderr, "leveler: target_lufs=%g\n", m.leveler.target_lufs);
    std::fprintf(stderr, "ceiling: %g dBTP, %g ms LA, atk=%g rel=%g\n",
                 m.ceiling.ceiling_dbtp, m.ceiling.lookahead_ms,
                 m.ceiling.attack_ms, m.ceiling.release_ms);

    assert(m.schema_version == 2);
    assert(m.effect_name == "Axon");
    assert(m.sample_rate == 44100);
    assert(m.sub_bundles.count("ssl_comp"));
    assert(!m.sub_bundles.count("la2a"));
    assert(!m.sub_bundles.count("saturator"));   // stage removed 2026-07
    assert(m.amt_ssl_comp.wet_mix_max == 1.0f);
    assert(m.leveler.target_lufs == -14.0f);
    assert(m.ceiling.ceiling_dbtp == -1.0f);
    std::fprintf(stderr, "[composite_meta] PASS\n");
    return 0;
}
