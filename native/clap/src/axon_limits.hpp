#pragma once

// Shared compile-time constants for the composite Axon plugin.
//
// These live in a header (rather than at the top of axon_plugin.cpp) so the
// tests that pin cross-file contracts compile against the REAL values instead
// of hand-copied mirrors that silently rot:
//   - tests/test_autoeq_param_guard.cpp   (kEqParamsStorage)
//   - tests/test_ssl_hop_contract.cpp     (kSslHop vs the shipped ssl_comp
//                                          bundle's trace_len/receptive_field)

namespace nablafx_axon {

// All ORT sessions in this plugin process audio in fixed kBlockSize chunks,
// which matches the auto-EQ controller's cond_block_size. Changing this
// requires re-exporting the ONNX bundles at the new block size (entry_init
// rejects a bundle whose declared block_size disagrees).
constexpr int kBlockSize  = 128;

// SSL bus comp accumulator size — must be a multiple of kBlockSize. Larger
// values cut CPU proportionally (1 ORT call per kSslHop samples instead of
// 1 per kBlockSize) at the cost of (kSslHop - kBlockSize) extra latency.
//
// CRITICAL CONSTRAINT: `kSslHop <= trace_len - RF` so every ring shift
// preserves at least RF samples of past context for the model's causal
// convolutions. Asserted at activate, and pinned against the shipped bundle
// by tests/test_ssl_hop_contract.cpp (the shipped margin is exactly 0:
// 1655 - 631 == 1024).
//
// The wet output is delayed by `kSslHop - kBlockSize` samples relative to
// the dry signal at the blend step. We compensate via a per-channel dry
// delay ring (see ssl_comp_dry_delay) so the wet/dry mix is sample-aligned.
constexpr int kSslHop     = 1024;

// The accumulator cadence (ssl_comp_in_fill advances kBlockSize per flush)
// and the dry-delay ring length (kSslHop - kBlockSize) both assume whole
// blocks per hop; a non-multiple would silently desynchronize the wet/dry
// alignment rather than fail to build.
static_assert(kSslHop % kBlockSize == 0,
              "kSslHop must be a multiple of kBlockSize");

// Size of the fixed stack arrays the audio thread stages auto-EQ control
// params through (eq_params_*_storage / autoeq_held_* in axon_plugin.cpp).
// plugin_activate rejects any bundle declaring num_control_params greater
// than this, otherwise the controller would write past the arrays on the
// audio thread (see tests/test_autoeq_param_guard.cpp).
constexpr int kEqParamsStorage = 64;

}  // namespace nablafx_axon
