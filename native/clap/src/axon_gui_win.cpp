// axon_gui_win.cpp — Windows WebView2 implementation of the axon_gui.h C ABI
// (Phase 3 of the Windows/Linux port).
//
// Architecture, mirroring axon_gui.mm one-to-one:
//
//   * WebView        WKWebView            -> ICoreWebView2 (Edge WebView2)
//   * Embedding      NSView addSubview    -> our own child HWND, SetParent'ed
//                                            into the host's window
//                                            (CLAP_WINDOW_API_WIN32;
//                                            clap_window.win32 is the HWND)
//   * JS -> native   WKScriptMessageHandler "axon"
//                                         -> WebView2 has no native
//                                            window.webkit.messageHandlers, so
//                                            AddScriptToExecuteOnDocumentCreated
//                                            injects the unit-tested shim
//                                            (axon::gui::webview2_bootstrap_js)
//                                            BEFORE any page script runs; it
//                                            forwards postMessage payloads as
//                                            JSON strings through
//                                            chrome.webview.postMessage, and
//                                            WebMessageReceived decodes them
//                                            with the shared
//                                            axon::gui::decode_bridge_message
//   * native -> JS   evaluateJavaScript   -> ICoreWebView2::ExecuteScript
//   * main thread    dispatch to main queue
//                                         -> not needed: CLAP guarantees all
//                                            gui-extension calls on the host
//                                            main (UI) thread, and WebView2
//                                            delivers its completion/event
//                                            callbacks on that same STA
//                                            thread via the host's Win32
//                                            message loop
//
// ASYNC BRING-UP. Unlike WKWebView/WebKitWebView, a WebView2 is created
// asynchronously (environment -> controller -> webview, each via a COM
// completion callback pumped by the host's message loop). axon_gui_create()
// therefore returns a live state whose webview materializes a few pump cycles
// later; set_parent/show/send_init calls that arrive earlier are recorded in
// the state and replayed when the controller lands (the same buffering idea
// the mac impl uses for pre-page-load init JS). If the WebView2 runtime is
// missing, environment creation fails and the plugin simply keeps running
// with the host's generic param UI (a user-facing runtime installer story is
// tracked in docs/future/active/windows-linux-builds.md).
//
// DLL note: we link WebView2LoaderStatic.lib (the SDK's static loader), so
// the .clap has NO extra DLL to stage next to it (package_windows.py stays
// unchanged; the only dependent DLL remains onnxruntime.dll).

#include "axon_gui.h"

#include <windows.h>

#include <objbase.h>

#include <WebView2.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "axon_gui_bridge.hpp"
#include "axon_gui_js.hpp"

// ---------------------------------------------------------------------------
// Small COM helpers (avoids a WRL dependency; clang-cl friendly)
// ---------------------------------------------------------------------------

namespace {

// Minimal COM completion/event handler: implements one interface whose only
// non-IUnknown method is Invoke(Args...), forwarding to a std::function.
template <typename Iface, typename... Args>
class ComHandler final : public Iface {
public:
    using Fn = std::function<HRESULT(Args...)>;
    explicit ComHandler(Fn fn) : fn_(std::move(fn)) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(Iface)) {
            *ppv = static_cast<Iface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override {
        return fn_(args...);
    }

private:
    virtual ~ComHandler() = default;
    std::atomic<ULONG> refs_{1};
    Fn fn_;
};

template <typename Iface, typename... Args>
ComHandler<Iface, Args...>* make_handler(
    std::function<HRESULT(Args...)> fn) {
    return new ComHandler<Iface, Args...>(std::move(fn));
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                      (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                        &out[0], n);
    return out;
}

std::string narrow(const wchar_t* utf16) {
    if (!utf16 || !*utf16) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 1) return {};
    std::string out((size_t)n, '\0');  // n includes the terminating NUL
    WideCharToMultiByte(CP_UTF8, 0, utf16, -1, &out[0], n, nullptr, nullptr);
    out.resize((size_t)n - 1);  // drop the NUL
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal state (mirrors AxonGUIState in axon_gui.mm)
// ---------------------------------------------------------------------------

struct AxonGUIState {
    // Callbacks into the plugin
    void* plugin_ptr = nullptr;
    void (*on_param_change)(void*, const char*, float) = nullptr;
    void (*on_order_change)(void*, const int*, int)    = nullptr;

    std::string resources_dir;

    // Win32 / WebView2 objects
    HWND                    hwnd       = nullptr;  // our child window
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2*           webview    = nullptr;
    EventRegistrationToken   msg_token{};
    EventRegistrationToken   nav_token{};

    bool com_initialized = false;  // we must pair CoUninitialize
    bool visible         = true;

    // Buffered init JS until the page finishes loading (same as the mac impl).
    std::string pending_init_js;
    bool        page_loaded = false;
};

namespace {

// Async-callback lifetime guard: COM completion handlers capture the state
// pointer and may fire after axon_gui_destroy() (e.g. host opens and
// immediately closes the window while the environment is still booting).
// All ABI calls and all WebView2 callbacks happen on the one host UI (STA)
// thread, so a plain set with no locking is correct here.
std::set<AxonGUIState*>& live_states() {
    static std::set<AxonGUIState*> s;
    return s;
}
bool is_live(AxonGUIState* st) {
    return live_states().count(st) != 0;
}

constexpr wchar_t kWndClass[] = L"AxonGuiHostWindow";

void resize_controller(AxonGUIState* st) {
    if (!st->controller || !st->hwnd) return;
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    st->controller->put_Bounds(rc);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE) {
        auto* st = reinterpret_cast<AxonGUIState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st && is_live(st)) resize_controller(st);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ensure_wnd_class() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    registered = true;
    return true;
}

// %LOCALAPPDATA%\Axon\WebView2 — the WebView2 user-data folder. The default
// (next to the host .exe) is typically not writable under Program Files.
std::wstring user_data_dir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n =
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(buf);
    dir += L"\\Axon";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\WebView2";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void run_js(AxonGUIState* st, const std::string& js) {
    if (!st->webview) return;
    st->webview->ExecuteScript(widen(js).c_str(), nullptr);
}

void on_web_message(AxonGUIState* st,
                    ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR wjson = nullptr;
    // The bootstrap shim posts JSON.stringify(...) — i.e. a STRING message —
    // so TryGetWebMessageAsString succeeds and yields exactly the JSON text
    // the shared decoder expects.
    if (FAILED(args->TryGetWebMessageAsString(&wjson)) || !wjson) return;
    const std::string json = narrow(wjson);
    CoTaskMemFree(wjson);

    axon::gui::BridgeMessage msg;
    if (axon::gui::decode_bridge_message(json.c_str(), msg)) {
        switch (msg.kind) {
            case axon::gui::BridgeMessage::Kind::SetParam:
                if (st->on_param_change)
                    st->on_param_change(st->plugin_ptr, msg.id.c_str(),
                                        (float)msg.value);
                break;
            case axon::gui::BridgeMessage::Kind::Reorder:
                if (st->on_order_change)
                    st->on_order_change(st->plugin_ptr, msg.order.data(),
                                        (int)msg.order.size());
                break;
            default:
                break;
        }
    }
}

void navigate_to_ui(AxonGUIState* st) {
    const std::string html_path = st->resources_dir + "/ui/index.html";
    const DWORD attrs = GetFileAttributesW(widen(html_path).c_str());
    const bool exists = attrs != INVALID_FILE_ATTRIBUTES &&
                        !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    if (exists) {
        const std::string url = axon::gui::file_url_from_path(html_path);
        st->webview->Navigate(widen(url).c_str());
    } else {
        // Same fallback page as the mac impl.
        st->webview->NavigateToString(
            L"<html><body style='background:#111;color:#f66;"
            L"font-family:monospace;padding:20px'>"
            L"<h2>Axon GUI</h2>"
            L"<p>Could not load index.html from resources.</p>"
            L"</body></html>");
    }
}

void on_controller_created(AxonGUIState* st,
                           ICoreWebView2Controller* controller) {
    controller->AddRef();
    st->controller = controller;

    if (FAILED(controller->get_CoreWebView2(&st->webview)) || !st->webview) {
        return;  // keep the (blank) child window; plugin stays usable
    }

    // JS -> native: the page's postMessage arrives here as a JSON string.
    st->webview->add_WebMessageReceived(
        make_handler<ICoreWebView2WebMessageReceivedEventHandler,
                     ICoreWebView2*,
                     ICoreWebView2WebMessageReceivedEventArgs*>(
            [st](ICoreWebView2*,
                 ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (is_live(st) && args) on_web_message(st, args);
                return S_OK;
            }),
        &st->msg_token);

    // Page-loaded notification (mac: didFinishNavigation) — flush buffered
    // init JS.
    st->webview->add_NavigationCompleted(
        make_handler<ICoreWebView2NavigationCompletedEventHandler,
                     ICoreWebView2*,
                     ICoreWebView2NavigationCompletedEventArgs*>(
            [st](ICoreWebView2*,
                 ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                if (!is_live(st)) return S_OK;
                st->page_loaded = true;
                if (!st->pending_init_js.empty()) {
                    std::string js;
                    js.swap(st->pending_init_js);
                    run_js(st, js);
                }
                return S_OK;
            }),
        &st->nav_token);

    // Install the messageHandlers.axon shim BEFORE navigating, so it runs
    // ahead of every page script; then load ui/index.html.
    st->webview->AddScriptToExecuteOnDocumentCreated(
        widen(axon::gui::webview2_bootstrap_js()).c_str(),
        make_handler<
            ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
            HRESULT, LPCWSTR>([st](HRESULT, LPCWSTR) -> HRESULT {
            if (is_live(st) && st->webview) navigate_to_ui(st);
            return S_OK;
        }));

    resize_controller(st);
    st->controller->put_IsVisible(st->visible ? TRUE : FALSE);
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
    if (!ensure_wnd_class()) return nullptr;

    auto* state = new AxonGUIState();
    state->plugin_ptr      = plugin_ptr;
    state->on_param_change = on_param_change;
    state->on_order_change = on_order_change;
    state->resources_dir   = resources_dir ? resources_dir : "";

    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    state->com_initialized = SUCCEEDED(co);  // S_OK or S_FALSE: pair; changed-mode: don't

    uint32_t w = 0, h = 0;
    axon_gui_get_size(&w, &h);
    // Created as a hidden top-level; set_parent() turns it into a WS_CHILD of
    // the host window (mac analogue: the container NSView before addSubview).
    state->hwnd = CreateWindowExW(
        0, kWndClass, L"Axon", WS_POPUP, 0, 0, (int)w, (int)h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!state->hwnd) {
        if (state->com_initialized) CoUninitialize();
        delete state;
        return nullptr;
    }
    SetWindowLongPtrW(state->hwnd, GWLP_USERDATA,
                      (LONG_PTR)state);

    live_states().insert(state);

    const std::wstring data_dir = user_data_dir();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data_dir.empty() ? nullptr : data_dir.c_str(), nullptr,
        make_handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                     HRESULT, ICoreWebView2Environment*>(
            [state](HRESULT result,
                    ICoreWebView2Environment* env) -> HRESULT {
                if (!is_live(state)) return S_OK;
                if (FAILED(result) || !env) {
                    std::fprintf(stderr,
                                 "[axon-gui] WebView2 environment failed "
                                 "(0x%08lx) — is the WebView2 runtime "
                                 "installed?\n",
                                 (unsigned long)result);
                    return S_OK;  // headless-degraded; window stays blank
                }
                env->CreateCoreWebView2Controller(
                    state->hwnd,
                    make_handler<
                        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                        HRESULT, ICoreWebView2Controller*>(
                        [state](HRESULT result2,
                                ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (!is_live(state)) return S_OK;
                            if (FAILED(result2) || !ctrl) return S_OK;
                            on_controller_created(state, ctrl);
                            return S_OK;
                        }));
                return S_OK;
            }));
    if (FAILED(hr)) {
        // No WebView2 runtime (or loader failure): report "no GUI" so hosts
        // fall back to their generic parameter UI (same contract as the
        // headless stub).
        std::fprintf(stderr,
                     "[axon-gui] CreateCoreWebView2EnvironmentWithOptions "
                     "failed (0x%08lx)\n",
                     (unsigned long)hr);
        axon_gui_destroy(state);
        return nullptr;
    }

    return state;
}

void axon_gui_destroy(AxonGUIState* gui) {
    if (!gui) return;
    live_states().erase(gui);

    if (gui->webview) {
        gui->webview->remove_WebMessageReceived(gui->msg_token);
        gui->webview->remove_NavigationCompleted(gui->nav_token);
        gui->webview->Release();
        gui->webview = nullptr;
    }
    if (gui->controller) {
        gui->controller->Close();
        gui->controller->Release();
        gui->controller = nullptr;
    }
    if (gui->hwnd) {
        SetWindowLongPtrW(gui->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(gui->hwnd);
        gui->hwnd = nullptr;
    }
    if (gui->com_initialized) CoUninitialize();
    delete gui;
}

bool axon_gui_set_parent(AxonGUIState* gui, void* hwnd_ptr) {
    if (!gui || !hwnd_ptr || !gui->hwnd) return false;

    HWND parent = (HWND)hwnd_ptr;
    // WS_POPUP -> WS_CHILD before SetParent (required combination).
    LONG_PTR style = GetWindowLongPtrW(gui->hwnd, GWL_STYLE);
    style &= ~(LONG_PTR)WS_POPUP;
    style |= WS_CHILD;
    SetWindowLongPtrW(gui->hwnd, GWL_STYLE, style);
    if (!SetParent(gui->hwnd, parent)) return false;

    uint32_t w = 0, h = 0;
    axon_gui_get_size(&w, &h);
    MoveWindow(gui->hwnd, 0, 0, (int)w, (int)h, TRUE);
    ShowWindow(gui->hwnd, SW_SHOW);
    resize_controller(gui);
    return true;
}

void axon_gui_show(AxonGUIState* gui) {
    if (!gui) return;
    gui->visible = true;
    if (gui->hwnd) ShowWindow(gui->hwnd, SW_SHOW);
    if (gui->controller) gui->controller->put_IsVisible(TRUE);
}

void axon_gui_hide(AxonGUIState* gui) {
    if (!gui) return;
    gui->visible = false;
    if (gui->hwnd) ShowWindow(gui->hwnd, SW_HIDE);
    if (gui->controller) gui->controller->put_IsVisible(FALSE);
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
    if (!gui->page_loaded) {
        gui->pending_init_js = std::move(js);
    } else {
        run_js(gui, js);
    }
}

void axon_gui_eval_js(AxonGUIState* gui, const char* js) {
    if (!gui || !js) return;
    if (!gui->webview || !gui->page_loaded) return;
    run_js(gui, js);
}

void axon_gui_notify_param(AxonGUIState* gui, const char* param_id,
                           float value) {
    if (!gui || !param_id) return;
    if (!gui->webview || !gui->page_loaded) return;
    run_js(gui, axon::build_set_param_js(param_id, value));
}

}  // extern "C"
