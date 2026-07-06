// axon_gui_gtk.cpp — Linux webkit2gtk implementation of the axon_gui.h C ABI
// (Phase 3 of the Windows/Linux port).
//
// Architecture, mirroring axon_gui.mm one-to-one:
//
//   * WebView        WKWebView            -> WebKitWebView (webkit2gtk 4.0/4.1)
//   * Embedding      NSView addSubview    -> GtkPlug XEmbed into the host's
//                                            X11 window (CLAP_WINDOW_API_X11;
//                                            clap_window.x11 is the XID)
//   * JS -> native   WKScriptMessageHandler "axon"
//                                         -> webkit_user_content_manager_
//                                            register_script_message_handler
//                                            ("axon"): the page's existing
//                                            window.webkit.messageHandlers.
//                                            axon.postMessage(...) works
//                                            UNCHANGED; the JSCValue payload
//                                            is serialized with
//                                            jsc_value_to_json and decoded by
//                                            the shared, unit-tested
//                                            axon::gui::decode_bridge_message
//   * native -> JS   evaluateJavaScript   -> webkit_web_view_evaluate_javascript
//                                            (4.1 >= 2.40; run_javascript on 4.0)
//   * main thread    dispatch_sync/async to the main queue
//                                         -> g_main_context_invoke_full on the
//                                            GLib default main context (see
//                                            THREADING below)
//
// THREADING. On macOS the host process always runs the Cocoa main loop, so
// axon_gui.mm can marshal everything onto dispatch_get_main_queue(). Linux
// hosts have no such guarantee: GTK-based hosts iterate the GLib default main
// context themselves, but most DAWs do not. So this backend lazily spawns ONE
// process-wide fallback thread that runs g_main_loop_run() on the default
// context. GLib main-context ownership arbitrates between the two cases: in a
// GTK host our fallback loop never acquires the context (the host's loop owns
// it) and our dispatched closures run on the host's main thread; elsewhere the
// fallback thread owns the context and runs both the closures and all
// GTK/WebKit sources. Either way, every GTK/WebKit call below happens on the
// single thread that owns the default context, which is GTK's threading rule.
// The plugin-side callbacks (on_param_change/on_order_change) consequently
// fire on that thread; axon_plugin.cpp routes them into mutex-guarded queues,
// so that is safe by design.
//
// RUNTIME CAVEATS (compile-verified in CI; hands-on host validation is
// tracked in docs/future/active/windows-linux-builds.md):
//   * XEmbed (GtkPlug) requires an X11 host window — under Wayland-only hosts
//     the GUI is unavailable (plugin still runs; generic params).
//   * gtk_disable_setlocale() is called before gtk_init so GTK cannot flip
//     LC_NUMERIC under the host (would corrupt %g formatting in the bridge).
//   * Linking webkit2gtk makes it a hard runtime dependency of the .clap;
//     build with -DAXON_GUI=OFF for a dependency-free headless plugin.

#include "axon_gui.h"

#include <gtk/gtk.h>
#include <gtk/gtkx.h>  // GtkPlug (X11-only widget)
#include <webkit2/webkit2.h>

#include <X11/Xlib.h>  // XInitThreads

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "axon_gui_bridge.hpp"
#include "axon_gui_js.hpp"

// ---------------------------------------------------------------------------
// Internal state (mirrors AxonGUIState in axon_gui.mm)
// ---------------------------------------------------------------------------

struct AxonGUIState {
    // Callbacks into the plugin
    void* plugin_ptr = nullptr;
    void (*on_param_change)(void*, const char*, float) = nullptr;
    void (*on_order_change)(void*, const int*, int)    = nullptr;

    std::string resources_dir;

    // GTK/WebKit objects — touched only on the context-owning thread.
    GtkWidget*                web_view    = nullptr;  // ref_sink'ed
    GtkWidget*                plug_window = nullptr;  // created in set_parent
    WebKitUserContentManager* ucm         = nullptr;  // owned ref

    // Buffered init JS until the page finishes loading (same as the mac impl).
    std::string pending_init_js;
    bool        page_loaded = false;
};

namespace {

// ---------------------------------------------------------------------------
// Main-context marshalling (the GLib analogue of dispatch_sync/async).
// ---------------------------------------------------------------------------

struct SyncClosure {
    std::function<void()>   fn;
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    done = false;
};

gboolean run_sync_closure(gpointer data) {
    auto* c = static_cast<SyncClosure*>(data);
    c->fn();
    {
        std::lock_guard<std::mutex> lk(c->mtx);
        c->done = true;
    }
    c->cv.notify_one();
    return G_SOURCE_REMOVE;
}

// Run `fn` on the thread owning the default main context and wait for it.
// If the calling thread already owns the context, run inline (no deadlock).
void invoke_sync(std::function<void()> fn) {
    if (g_main_context_is_owner(g_main_context_default())) {
        fn();
        return;
    }
    SyncClosure c;
    c.fn = std::move(fn);
    g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, run_sync_closure,
                               &c, nullptr);
    std::unique_lock<std::mutex> lk(c.mtx);
    c.cv.wait(lk, [&] { return c.done; });
}

struct AsyncClosure {
    std::function<void()> fn;
};

gboolean run_async_closure(gpointer data) {
    static_cast<AsyncClosure*>(data)->fn();
    return G_SOURCE_REMOVE;
}

void destroy_async_closure(gpointer data) {
    delete static_cast<AsyncClosure*>(data);
}

void invoke_async(std::function<void()> fn) {
    g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, run_async_closure,
                               new AsyncClosure{std::move(fn)},
                               destroy_async_closure);
}

// ---------------------------------------------------------------------------
// One-time GTK bring-up + fallback main-loop thread (see THREADING above).
// ---------------------------------------------------------------------------

bool ensure_gtk() {
    static bool initialized = false;
    static bool ok          = false;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    if (initialized) return ok;
    initialized = true;

    XInitThreads();
    gdk_set_allowed_backends("x11");  // GtkPlug/XEmbed is X11-only
    gtk_disable_setlocale();          // do NOT flip the host's LC_NUMERIC
    ok = gtk_init_check(nullptr, nullptr) != FALSE;
    if (!ok) {
        std::fprintf(stderr, "[axon-gui] gtk_init_check failed (no display?)\n");
        return false;
    }

    // Fallback pump: parks forever if a GTK host owns the default context.
    std::thread([] {
        GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
        g_main_loop_run(loop);  // never returns; thread lives with the process
    }).detach();
    return true;
}

// ---------------------------------------------------------------------------
// WebKit signal handlers
// ---------------------------------------------------------------------------

void on_script_message(WebKitUserContentManager* /*ucm*/,
                       WebKitJavascriptResult* jsres, gpointer user_data) {
    auto* state = static_cast<AxonGUIState*>(user_data);
    if (!state || !jsres) return;

    JSCValue* v = webkit_javascript_result_get_js_value(jsres);
    if (!v) return;
    char* json = jsc_value_to_json(v, 0);
    if (!json) return;

    axon::gui::BridgeMessage msg;
    if (axon::gui::decode_bridge_message(json, msg)) {
        switch (msg.kind) {
            case axon::gui::BridgeMessage::Kind::SetParam:
                if (state->on_param_change)
                    state->on_param_change(state->plugin_ptr, msg.id.c_str(),
                                           (float)msg.value);
                break;
            case axon::gui::BridgeMessage::Kind::Reorder:
                if (state->on_order_change)
                    state->on_order_change(state->plugin_ptr, msg.order.data(),
                                           (int)msg.order.size());
                break;
            default:
                break;
        }
    }
    g_free(json);
}

void run_js(AxonGUIState* state, const std::string& js) {
    if (!state->web_view) return;
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(state->web_view),
                                        js.c_str(), (gssize)js.size(),
                                        nullptr, nullptr, nullptr, nullptr,
                                        nullptr);
#else
    webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(state->web_view),
                                   js.c_str(), nullptr, nullptr, nullptr);
#endif
}

void on_load_changed(WebKitWebView* /*view*/, WebKitLoadEvent event,
                     gpointer user_data) {
    auto* state = static_cast<AxonGUIState*>(user_data);
    if (!state || event != WEBKIT_LOAD_FINISHED) return;
    state->page_loaded = true;
    if (!state->pending_init_js.empty()) {
        std::string js;
        js.swap(state->pending_init_js);
        run_js(state, js);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public C API implementation
// ---------------------------------------------------------------------------

extern "C" {

AxonGUIState* axon_gui_create(
    void*       plugin_ptr,
    const char* resources_dir,
    void      (*on_param_change)(void*, const char*, float),
    void      (*on_order_change)(void*, const int*, int)
) {
    if (!ensure_gtk()) return nullptr;

    auto* state = new AxonGUIState();
    state->plugin_ptr      = plugin_ptr;
    state->on_param_change = on_param_change;
    state->on_order_change = on_order_change;
    state->resources_dir   = resources_dir ? resources_dir : "";

    invoke_sync([state] {
        // --- User content manager + "axon" script message handler ---
        state->ucm = webkit_user_content_manager_new();
        g_signal_connect(state->ucm, "script-message-received::axon",
                         G_CALLBACK(on_script_message), state);
        webkit_user_content_manager_register_script_message_handler(
            state->ucm, "axon");

        // --- WebKitWebView ---
        GtkWidget* wv =
            webkit_web_view_new_with_user_content_manager(state->ucm);
        g_object_ref_sink(wv);  // keep alive until destroy (no parent yet)
        state->web_view = wv;

        // Same background the mac container view paints (#111111-ish).
        GdkRGBA bg = {0.067, 0.067, 0.067, 1.0};
        webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(wv), &bg);

        g_signal_connect(wv, "load-changed", G_CALLBACK(on_load_changed),
                         state);

        // --- Load ui/index.html (or the same fallback page as the mac impl) ---
        const std::string html_path = state->resources_dir + "/ui/index.html";
        if (g_file_test(html_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
            const std::string url = axon::gui::file_url_from_path(html_path);
            webkit_web_view_load_uri(WEBKIT_WEB_VIEW(wv), url.c_str());
        } else {
            webkit_web_view_load_html(
                WEBKIT_WEB_VIEW(wv),
                "<html><body style='background:#111;color:#f66;"
                "font-family:monospace;padding:20px'>"
                "<h2>Axon GUI</h2>"
                "<p>Could not load index.html from resources.</p>"
                "</body></html>",
                nullptr);
        }
    });

    return state;
}

void axon_gui_destroy(AxonGUIState* gui) {
    if (!gui) return;

    invoke_sync([gui] {
        if (gui->ucm) {
            g_signal_handlers_disconnect_by_data(gui->ucm, gui);
            webkit_user_content_manager_unregister_script_message_handler(
                gui->ucm, "axon");
        }
        if (gui->web_view) {
            g_signal_handlers_disconnect_by_data(gui->web_view, gui);
        }
        if (gui->plug_window) {
            gtk_widget_destroy(gui->plug_window);  // also unparents web_view
            gui->plug_window = nullptr;
        }
        if (gui->web_view) {
            g_object_unref(gui->web_view);
            gui->web_view = nullptr;
        }
        if (gui->ucm) {
            g_object_unref(gui->ucm);
            gui->ucm = nullptr;
        }
    });

    delete gui;
}

bool axon_gui_set_parent(AxonGUIState* gui, void* x11_window) {
    if (!gui || !x11_window) return false;

    bool result = false;
    invoke_sync([gui, x11_window, &result] {
        if (!gui->web_view) return;
        if (gui->plug_window) {  // reparent: drop the old plug first
            gtk_container_remove(GTK_CONTAINER(gui->plug_window),
                                 gui->web_view);
            gtk_widget_destroy(gui->plug_window);
            gui->plug_window = nullptr;
        }

        // clap_window.x11 (an XID) arrives cast to void* by axon_plugin.cpp.
        const Window socket_id = (Window)(uintptr_t)x11_window;
        GtkWidget*   plug      = gtk_plug_new(socket_id);
        if (!plug) return;

        uint32_t w = 0, h = 0;
        axon_gui_get_size(&w, &h);
        gtk_window_set_default_size(GTK_WINDOW(plug), (gint)w, (gint)h);
        gtk_container_add(GTK_CONTAINER(plug), gui->web_view);
        gtk_widget_show_all(plug);
        gui->plug_window = plug;
        result = true;
    });
    return result;
}

void axon_gui_show(AxonGUIState* gui) {
    if (!gui) return;
    invoke_async([gui] {
        if (gui->plug_window) gtk_widget_show(gui->plug_window);
    });
}

void axon_gui_hide(AxonGUIState* gui) {
    if (!gui) return;
    invoke_async([gui] {
        if (gui->plug_window) gtk_widget_hide(gui->plug_window);
    });
}

void axon_gui_get_size(uint32_t* w, uint32_t* h) {
    if (w) *w = 820;
    if (h) *h = 600;
}

void axon_gui_send_init(
    AxonGUIState*        gui,
    const AxonParamInfo* params,
    int                  n_params,
    const int*           order,
    int                  order_count
) {
    if (!gui) return;
    // Shared, unit-tested builder — byte-identical payload on all backends.
    std::string js =
        axon::gui::build_init_js(params, n_params, order, order_count);

    invoke_async([gui, js = std::move(js)] {
        if (!gui->page_loaded) {
            gui->pending_init_js = js;
        } else {
            run_js(gui, js);
        }
    });
}

void axon_gui_eval_js(AxonGUIState* gui, const char* js) {
    if (!gui || !js) return;
    std::string js_str = js;
    invoke_async([gui, js_str = std::move(js_str)] {
        if (!gui->web_view || !gui->page_loaded) return;
        run_js(gui, js_str);
    });
}

void axon_gui_notify_param(AxonGUIState* gui, const char* param_id,
                           float value) {
    if (!gui || !param_id) return;
    std::string js_str = axon::build_set_param_js(param_id, value);
    invoke_async([gui, js_str = std::move(js_str)] {
        if (!gui->web_view || !gui->page_loaded) return;
        run_js(gui, js_str);
    });
}

}  // extern "C"
