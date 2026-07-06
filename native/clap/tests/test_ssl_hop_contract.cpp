// Cross-file contract: kSslHop (src/axon_limits.hpp) vs the SHIPPED ssl_comp
// bundle's plugin_meta.json.
//
// plugin_activate requires `kSslHop <= trace_len - receptive_field` so every
// ring shift preserves at least RF samples of past context for the model's
// causal convolutions. The shipped bundle satisfies this with ZERO margin
// (trace_len=1655, rf=631 -> 1024 == kSslHop), so ANY re-export with a larger
// receptive field or shorter trace, or any bump of kSslHop, silently builds
// and then fails at activate — which, before the try/catch around activate,
// crashed the DAW. composite.py cannot check this (it doesn't know kSslHop),
// and no other test reads the real bundle's trace_len. This one pins the
// constraint against the actual shipped artifact.
//
// SSL_META_PATH is injected by CMake (like test_control_contract's META_PATH)
// and points at weights/axon_bundle/ssl_comp/plugin_meta.json. kSslHop comes
// from axon_limits.hpp — the same header the plugin compiles — so the constant
// cannot silently fork.

#include "../src/axon_limits.hpp"
#include "../src/meta.hpp"

#include <cassert>
#include <cstdio>

int main() {
    const nablafx::PluginMeta m = nablafx::load_meta(SSL_META_PATH);

    std::fprintf(stderr,
        "[ssl-hop] shipped ssl_comp: trace_len=%d receptive_field=%d "
        "kSslHop=%d margin=%d\n",
        m.trace_len, m.receptive_field, nablafx_axon::kSslHop,
        m.trace_len - m.receptive_field - nablafx_axon::kSslHop);

    // The bundle must actually declare the fields the constraint is built on.
    assert(m.trace_len > 0 && "shipped ssl_comp meta lacks trace_len");
    assert(m.receptive_field > 0 && "shipped ssl_comp meta lacks receptive_field");

    // The activate-time constraint, checked against the SHIPPED bundle at
    // test time instead of in the DAW at load time.
    assert(nablafx_axon::kSslHop <= m.trace_len - m.receptive_field &&
           "kSslHop > trace_len - receptive_field: this bundle would fail at "
           "activate (hop-rate discontinuity guard). Lower kSslHop or "
           "re-export ssl_comp with a larger trace_len.");

    std::fprintf(stderr, "[ssl-hop] PASS\n");
    std::fprintf(stderr, "ALL SSL-HOP CONTRACT TESTS PASSED\n");
    return 0;
}
