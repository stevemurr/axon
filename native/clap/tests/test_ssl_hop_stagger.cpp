// Contract: the ssl_comp hop-phase STAGGER keeps the two channels' TCN
// forwards off the same host block (issue #22 — the per-block CPU spike).
//
// The plugin accumulates kBlockSize input samples per host block and runs one
// ORT forward every kSslHop samples (axon_plugin.cpp, flush loop). With every
// channel's accumulator starting at fill=0, both channels cross the hop
// boundary on the SAME block → two forwards back-to-back → the dominant
// per-block spike. plugin_activate offsets channel 1 to fill=kSslHop/2 with a
// one-shot "priming flush" (seed the ring, suppress the output) so its forward
// lands half a hop away.
//
// This test does NOT spin up ORT or a live plugin — it mirrors the flush
// cadence (axon_plugin.cpp:1922-1962) against the REAL kSslHop / kBlockSize
// from axon_limits.hpp, so it pins the SCHEDULING invariant the fix relies on
// and fails if the offset is dropped, the priming flush is removed, or kSslHop
// is changed to a value where kSslHop/2 no longer lands on a flush boundary.

#include "../src/axon_limits.hpp"

#include <cassert>
#include <cstdio>

using nablafx_axon::kBlockSize;
using nablafx_axon::kSslHop;

namespace {

// One channel's hop-accumulator state, mirroring ChannelChain's ssl_comp_*.
struct HopChan {
    int fill;
    int prime;
};

enum Flush { NONE = 0, PRIMING = 1, FORWARD = 2 };

// Advance one host block. Mirrors the flush branch exactly:
//   fill += kBlockSize;
//   if (fill >= kSslHop) { fill = 0; if (prime>0) --prime;  // priming: no ORT
//                                    else run-forward }      // real forward
// Returns which kind of flush (if any) this block triggered.
Flush step_block(HopChan& c) {
    c.fill += kBlockSize;
    if (c.fill >= kSslHop) {
        c.fill = 0;
        if (c.prime > 0) { --c.prime; return PRIMING; }
        return FORWARD;
    }
    return NONE;
}

}  // namespace

int main() {
    // The offset must be a whole number of block-sized flushes, or the
    // pre-loaded accumulator would never align to a hop boundary (and would
    // index past the kSslHop-sized accum buffer). Mirrors the static_assert
    // guarding the offset in plugin_activate.
    static_assert((kSslHop / 2) % kBlockSize == 0,
                  "kSslHop/2 must land on a kBlockSize flush boundary");

    const int hop_blocks = kSslHop / kBlockSize;   // host blocks per hop
    const int N = hop_blocks * 64;                 // 64 hops — plenty

    // ---- Baseline (the bug): both channels start at phase 0. ----
    {
        HopChan a{0, 0}, b{0, 0};
        int collisions = 0, a_fwd = 0, b_fwd = 0;
        for (int blk = 0; blk < N; ++blk) {
            const bool fa = step_block(a) == FORWARD;
            const bool fb = step_block(b) == FORWARD;
            a_fwd += fa; b_fwd += fb;
            if (fa && fb) ++collisions;
        }
        std::fprintf(stderr,
            "[stagger] baseline phase-0: a_fwd=%d b_fwd=%d collisions=%d\n",
            a_fwd, b_fwd, collisions);
        // Every hop boundary carries BOTH forwards — that IS the spike.
        assert(collisions == a_fwd && collisions == b_fwd &&
               "baseline should collide on every hop (documents the bug)");
        assert(collisions > 0);
    }

    // ---- Fixed: channel 1 offset by kSslHop/2 with one priming flush. ----
    {
        HopChan a{0, 0};
        HopChan b{kSslHop / 2, 1};   // exactly what plugin_activate installs
        int collisions = 0, a_fwd = 0, b_fwd = 0, b_priming = 0;
        int first_a = -1, first_b = -1;
        for (int blk = 0; blk < N; ++blk) {
            const Flush ea = step_block(a);
            const Flush eb = step_block(b);
            a_fwd += (ea == FORWARD);
            b_fwd += (eb == FORWARD);
            b_priming += (eb == PRIMING);
            if (ea == FORWARD && first_a < 0) first_a = blk;
            if (eb == FORWARD && first_b < 0) first_b = blk;
            if (ea == FORWARD && eb == FORWARD) ++collisions;
        }
        std::fprintf(stderr,
            "[stagger] fixed offset=kSslHop/2: a_fwd=%d b_fwd=%d collisions=%d "
            "first_a=%d first_b=%d b_priming=%d\n",
            a_fwd, b_fwd, collisions, first_a, first_b, b_priming);

        // The whole point: the two channels NEVER run a real forward on the
        // same block, so no block ever carries two forwards.
        assert(collisions == 0 &&
               "staggered channels must never share a forward block");

        // Exactly one priming (dry, suppressed) flush is consumed, once.
        assert(b_priming == 1 &&
               "offset channel must consume exactly one priming flush");

        // Both channels run the steady-state cadence; the offset channel does
        // one fewer real forward over the window (its first boundary was the
        // suppressed priming flush).
        assert(a_fwd > 0 && b_fwd > 0);
        assert(a_fwd - b_fwd == 1 &&
               "offset channel drops exactly its first (priming) forward");

        // Forwards are maximally separated: b lands half a hop after a.
        const int sep = (first_b - first_a) % hop_blocks;
        assert(sep == hop_blocks / 2 &&
               "offset channel's forward must sit half a hop from channel 0's");
    }

    std::fprintf(stderr, "[stagger] PASS\n");
    std::fprintf(stderr, "ALL SSL-HOP-STAGGER TESTS PASSED\n");
    return 0;
}
