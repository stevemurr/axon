// ort_shape.hpp — tiny ONNX Runtime shape probe shared by the plugin and tests.
//
// The auto-EQ controller graph is a BATCHED export: audio_in is [2, 1, T] so
// both channels run in one ORT call (OrtMiniSession::run_controller feeds a
// fixed {2,1,T} buffer and reads batch-2 param offsets). A batch-1 export — the
// default shape produced by nablafx-export — would feed that {2,1,T} buffer
// into a {1,1,T} input and throw INSIDE run_controller on the audio thread,
// which has no try/catch: std::terminate, i.e. a host crash. plugin_activate
// probes the batch dim with this helper and refuses activation on a mismatch,
// where the activate-time try/catch turns it into a clean failure (issue #24).

#pragma once

#include <onnxruntime_cxx_api.h>

#include <cstdint>
#include <cstring>

namespace nablafx {

// The batch dimension (shape[0]) that `input_name` declares on the session's
// model, or -1 if the input is absent or its rank is 0. A dynamic batch dim is
// reported by ORT as -1 as well; the caller treats "not exactly 2" as invalid.
inline int64_t ort_input_batch(const Ort::Session& sess, const char* input_name) {
    Ort::AllocatorWithDefaultOptions alloc;
    const size_t n = sess.GetInputCount();
    for (size_t i = 0; i < n; ++i) {
        Ort::AllocatedStringPtr nm = sess.GetInputNameAllocated(i, alloc);
        if (std::strcmp(nm.get(), input_name) == 0) {
            const std::vector<int64_t> shape =
                sess.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            return shape.empty() ? -1 : shape[0];
        }
    }
    return -1;
}

}  // namespace nablafx
