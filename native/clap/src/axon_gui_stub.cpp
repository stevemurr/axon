// axon_gui_stub.cpp — headless no-op implementation of the axon_gui.h C ABI
// for platforms without a native GUI backend yet (Linux/Windows Phase 1-2 of
// the port; the real backends — webkit2gtk / WebView2 — are Phase 3).
//
// The plugin is fully usable headless: every control is exposed as a CLAP
// param, so hosts show their generic UI. axon_plugin.cpp's CLAP gui
// extension only advertises CLAP_WINDOW_API_COCOA, so on other platforms
// hosts never even attempt to create the embedded GUI; these stubs exist so
// the plugin links and so any stray call is a safe no-op.
//
// Compiled ONLY on non-Apple platforms (see CMakeLists.txt); macOS builds
// keep src/axon_gui.mm untouched.

#include "axon_gui.h"

#include <cstddef>

extern "C" {

AxonGUIState* axon_gui_create(
    void* /*plugin_ptr*/,
    const char* /*resources_dir*/,
    void (*/*on_param_change*/)(void*, const char*, float),
    void (*/*on_order_change*/)(void*, const int*, int)) {
    return nullptr;  // "no GUI available" — gui_create() reports failure
}

void axon_gui_destroy(AxonGUIState* /*gui*/) {}

bool axon_gui_set_parent(AxonGUIState* /*gui*/, void* /*ns_view_ptr*/) {
    return false;
}

void axon_gui_show(AxonGUIState* /*gui*/) {}
void axon_gui_hide(AxonGUIState* /*gui*/) {}

// Same fixed size the macOS implementation reports (axon_gui.mm).
void axon_gui_get_size(uint32_t* w, uint32_t* h) {
    if (w) *w = 820;
    if (h) *h = 600;
}

void axon_gui_send_init(AxonGUIState* /*gui*/, const AxonParamInfo* /*params*/,
                        int /*n_params*/, const int* /*order*/,
                        int /*order_count*/) {}

void axon_gui_notify_param(AxonGUIState* /*gui*/, const char* /*param_id*/,
                           float /*value*/) {}

void axon_gui_eval_js(AxonGUIState* /*gui*/, const char* /*js*/) {}

}  // extern "C"
