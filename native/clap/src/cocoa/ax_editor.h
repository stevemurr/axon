// ax_editor.h: the native editor's one extra entry point, for the headless snapshot tool and tests.
#pragma once
#include "../axon_gui.h"

#ifdef __cplusplus
extern "C" {
#endif
// The editor's root NSView (as void*), or NULL. Valid until axon_gui_destroy.
void* axon_gui_native_view(AxonGUIState* gui);
// Run one refresh now (what the 30 Hz timer does).
void axon_gui_native_refresh(AxonGUIState* gui);
// Show a stage's panel, by StageID.
void axon_gui_native_select(AxonGUIState* gui, int stage);
#ifdef __cplusplus
}
#endif
