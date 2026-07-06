// axon_gui_stub.cpp — headless no-op implementation of the axon_gui.h C ABI.
// Used when no native GUI backend is available or wanted: -DAXON_GUI=OFF, or
// a Linux build without the webkit2gtk dev package (the real backends are
// axon_gui.mm / axon_gui_gtk.cpp / axon_gui_win.cpp — see CMakeLists.txt).
//
// The plugin is fully usable headless: every control is exposed as a CLAP
// param, so hosts show their generic UI. Stub builds compile axon_plugin.cpp
// with AXON_HAS_GUI=0, which withholds the CLAP gui extension entirely, so
// hosts never even attempt to create the embedded GUI; these stubs exist so
// the plugin links and so any stray call is a safe no-op.

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
