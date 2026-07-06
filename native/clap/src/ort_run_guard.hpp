#pragma once

#include <cstddef>
#include <stdexcept>

#include "meta.hpp"

namespace nablafx {

// Precondition checks + audio-output-length arithmetic for OrtSession::run().
//
// Factored out of run() (ort_session.cpp) into a header-only free function so
// tests/test_ort_session.cpp exercises the REAL guard instead of a hand-copied
// mirror of it: deleting or reordering these checks now fails the test.
//
// Returns the element count run() passes to Ort::Value::CreateTensor for the
// audio_out_ tensor. The `input_len < receptive_field` floor is load-bearing:
// without it, audio_out_len goes negative and the size_t cast turns it into a
// ~2^64-scale element count — the heap overflow the guard was added to fix.
// This header is deliberately ORT-free so tests need no onnxruntime dep.
inline std::size_t ort_run_out_len(const PluginMeta& meta, int max_input_len,
                                   int input_len) {
    if (input_len > max_input_len) {
        throw std::runtime_error(
            "OrtSession::run: input_len exceeds max configured block length");
    }
    if (input_len < meta.receptive_field) {
        throw std::runtime_error(
            "OrtSession::run: input_len must be at least receptive_field");
    }
    return static_cast<std::size_t>(input_len - (meta.receptive_field - 1));
}

}  // namespace nablafx
