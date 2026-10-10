#include "overlay.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "perf.hpp"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <gyrolib/gyrolib.h>
#include <gyrolib/overlay.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace gyroue::overlay {
namespace {

// ---------------------------------------------------------------- hook plumbing

// The original function of every vtable slot we ever patched, by slot address. Each slot is patched ONCE:
// if another hooker overwrites it later, it chains to us, and patching it again would make us call that
// hooker's function as our "original" while it calls us back: an endless loop (stack overflow in Cronos with
// SCSKiller's d3d12.dll proxy, 0.6.1). Every hook calls the original of the slot its object came through.
std::mutex g_slot_mutex;
std::unordered_map<void**, void*> g_slot_originals;

bool patch_vtable(void** vtable, int index, void* replacement, void** original) {
    void** const slot = &vtable[index];
    std::lock_guard lock(g_slot_mutex);
    if (g_slot_originals.count(slot) || *slot == replacement) return true;  // ours already (shared vtable)
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    g_slot_originals.emplace(slot, *slot);
    if (!*original) *original = *slot;  // fallback for objects whose vtable we never saw
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// The function our hook must call for this object: the original we saved for its vtable slot.
template <class T>
T original_of(void* object, int index, T fallback) {
    void** const slot = *static_cast<void***>(object) + index;
    std::lock_guard lock(g_slot_mutex);
    const auto it = g_slot_originals.find(slot);
    return it != g_slot_originals.end() ? reinterpret_cast<T>(it->second) : fallback;
}

// ---------------------------------------------------------------- state

using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);

using CreateFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
CreateFactory_t g_create_factory = nullptr, g_create_factory1 = nullptr;
CreateFactory2_t g_create_factory2 = nullptr;
CreateSwapChain_t g_create_swapchain = nullptr;
CreateSwapChainForHwnd_t g_create_swapchain_hwnd = nullptr;
Present_t g_present = nullptr;
Present1_t g_present1 = nullptr;
ResizeBuffers_t g_resize = nullptr;
ResizeBuffers1_t g_resize1 = nullptr;
WNDPROC g_wndproc = nullptr;

std::mutex g_mutex;                       // guards swapchain/queue handoff
IDXGISwapChain3* g_swapchain = nullptr;   // captured game swapchain (AddRef'd)
ID3D12CommandQueue* g_queue = nullptr;    // its DIRECT queue (AddRef'd)
HWND g_hwnd = nullptr;
std::atomic<gl_overlay*> g_overlay{nullptr};
std::atomic<bool> g_renderer_ready{false}, g_renderer_failed{false}, g_reinit{false};
bool g_subclassed = false;
std::atomic<UINT> g_menu_vk{0};  // virtual key of the panel shortcut (handled by polling)
bool g_menu_key_down = false;
LARGE_INTEGER g_freq{}, g_last_present{};
UINT g_buffer_count = 0;
DXGI_FORMAT g_buffer_format = DXGI_FORMAT_UNKNOWN;
DWORD g_render_thread = 0;
std::atomic<bool> g_buffers_held{false};  // GyroLib holds backbuffer references
// Diagnostics: panel visibility and render results are logged on change.
int32_t g_last_render_result = GL_OK;
uint32_t g_last_capture = 0;
bool g_last_panel_open = false;
void* g_foreign_swapchain = nullptr;

// ---------------------------------------------------------------- render worker
//
// GyroLib binds its DX12 renderer to ONE thread (init/render/before_resize/shutdown), but UE may move
// Present between threads (Cronos: after a ResizeBuffers, Present moved from one thread to another and
// every render failed with "render requires initialized render thread"), and resize from yet another
// thread (Returnal). So every GyroLib render call runs on our own worker thread: the presenting/resizing
// thread hands the job over and waits, which keeps the order (panel drawn before the game's Present).
// GyroLib must see a render every frame even while the panel is closed: it refuses/closes the panel when the
// render side is older than 250 ms (0.4.4 skipped closed frames and F10 stopped working).

std::mutex g_job_mutex;  // one job at a time (Present and ResizeBuffers may come from different threads)
HANDLE g_job_event = nullptr, g_done_event = nullptr;
void (*g_job)() = nullptr;

DWORD WINAPI worker_main(void*) {
    for (;;) {
        WaitForSingleObject(g_job_event, INFINITE);
        g_job();
        SetEvent(g_done_event);
    }
}

// Runs job on the render worker and waits for it. False if the worker is unavailable or stuck.
bool on_worker(void (*job)()) {
    std::lock_guard lock(g_job_mutex);
    if (g_renderer_failed) return false;
    if (!g_job_event) {
        g_job_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE thread = g_job_event && g_done_event ? CreateThread(nullptr, 0, &worker_main, nullptr, 0, &g_render_thread) : nullptr;
        if (!thread) {
            log("overlay: cannot start the render worker (error %lu); panel disabled", GetLastError());
            g_renderer_failed = true;
            return false;
        }
        CloseHandle(thread);
        log("overlay: render worker thread %lu", g_render_thread);
    }
    g_job = job;
    SetEvent(g_job_event);
    if (WaitForSingleObject(g_done_event, 5000) != WAIT_OBJECT_0) {
        log("overlay: render worker stuck for 5 s; panel disabled");
        g_renderer_failed = true;
        return false;
    }
    return true;
}

// Worker jobs read the captured swapchain under g_mutex.
void render_job() {
    gl_overlay* overlay = g_overlay.load();
    if (!overlay || g_renderer_failed) return;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(g_mutex);
        swapchain = g_swapchain, queue = g_queue, hwnd = g_hwnd;
    }
    if (g_reinit.exchange(false) && g_renderer_ready) {
        const int32_t r = gl_overlay_dx12_shutdown(overlay);
        log("overlay: renderer shutdown for re-init (%d)%s%s", r, r != GL_OK ? ": " : "", r != GL_OK ? gl_overlay_error() : "");
        g_renderer_ready = false;
        g_buffers_held = false;
    }
    if (!g_renderer_ready) {
        const gl_overlay_dx12_desc desc{sizeof(desc), GL_OVERLAY_ABI_VERSION, GL_OVERLAY_COLOR_SPACE_AUTO, 0, hwnd, swapchain, queue};
        const int32_t r = gl_overlay_dx12_init(overlay, &desc);
        if (r != GL_OK) {
            log("overlay: DX12 init failed (%d): %s; panel disabled", r, gl_overlay_error());
            g_renderer_failed = true;
            return;
        }
        g_renderer_ready = true;
        DXGI_SWAP_CHAIN_DESC1 d{};
        swapchain->GetDesc1(&d);
        g_buffer_count = d.BufferCount;
        g_buffer_format = d.Format;
        log("overlay: DX12 panel ready (render worker %lu, %u buffers, format %d)", g_render_thread, d.BufferCount,
            int(d.Format));
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = g_last_present.QuadPart ? double(now.QuadPart - g_last_present.QuadPart) / double(g_freq.QuadPart) : 1.0 / 60;
    g_last_present = now;
    if (dt <= 0 || dt > 1) dt = 1.0 / 60;
    const UINT dpi = GetDpiForWindow(hwnd);
    const int32_t r = gl_overlay_dx12_render(overlay, dt, dpi ? dpi / 96.0f : 1.0f);
    if (r == GL_IO_ERROR) {
        log("overlay: render device error: %s; panel disabled", gl_overlay_error());
        g_renderer_failed = true;
        return;
    }
    if (r != g_last_render_result) {
        log("overlay: render returned %d%s%s", r, r != GL_OK ? ": " : "", r != GL_OK ? gl_overlay_error() : "");
        g_last_render_result = r;
    }
    const uint32_t cap = gl_overlay_capture(overlay);
    if ((cap != 0) != (g_last_capture != 0)) log("overlay: panel %s (capture %u)", cap ? "drawn" : "hidden", cap);
    g_last_capture = cap;
    g_buffers_held = true;
    // Closed: nothing is drawn, so release the backbuffer references now; a resize then never has to
    // wait for GyroLib.
    if (cap == 0 && gl_overlay_dx12_before_resize(overlay) == GL_OK) g_buffers_held = false;
}

void before_resize_job() {
    gl_overlay* overlay = g_overlay.load();
    if (!overlay || !g_renderer_ready || !g_buffers_held) return;
    const int32_t r = gl_overlay_dx12_before_resize(overlay);
    if (r == GL_OK) g_buffers_held = false;
    log("overlay: before resize (%d)%s%s", r, r != GL_OK ? ": " : "", r != GL_OK ? gl_overlay_error() : "");
}

bool is_captured(IDXGISwapChain* sc) {
    std::lock_guard lock(g_mutex);
    return static_cast<IUnknown*>(sc) == static_cast<IUnknown*>(g_swapchain);
}

void render(IDXGISwapChain* sc) {
    if (!g_overlay.load() || g_renderer_failed) return;
    if (!is_captured(sc)) {
        if (g_foreign_swapchain != sc) {
            g_foreign_swapchain = sc;
            log("overlay: Present on a swapchain that wasn't captured (%p); panel not drawn there", static_cast<void*>(sc));
        }
        return;
    }
    on_worker(&render_job);
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        const int64_t start = perf::now();
        render(sc);
        perf::record(perf::Present, start);
    }
    return original_of(sc, 8, g_present)(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        const int64_t start = perf::now();
        render(sc);
        perf::record(perf::Present, start);
    }
    return original_of(sc, 22, g_present1)(sc, sync, flags, params);
}

// GyroLib's documented resize path: release backbuffer references before ResizeBuffers (on the worker,
// whatever thread resizes); the next render reacquires them. A changed count/format needs a re-init.
void before_resize(IDXGISwapChain* sc, UINT count, DXGI_FORMAT format) {
    if (!g_overlay.load() || !is_captured(sc) || !g_renderer_ready) return;
    if (g_buffers_held) on_worker(&before_resize_job);
    const bool changed = (count != 0 && count != g_buffer_count) || (format != DXGI_FORMAT_UNKNOWN && format != g_buffer_format);
    if (changed) {
        g_reinit = true;
        log("overlay: resize changes buffer count/format: re-init on next Present");
    }
}

// ReShade add-ons such as RenoDX can change the real buffer format behind the game's back (e.g. the game asks
// for RGB10A2 and gets RGBA16F). GyroLib needs a full re-init when count/format change, so compare against
// what the swapchain really has after the resize, not against the arguments.
void after_resize(IDXGISwapChain* sc, HRESULT hr, const char* what, UINT count, UINT w, UINT h, DXGI_FORMAT format) {
    DXGI_SWAP_CHAIN_DESC d{};
    const bool have = SUCCEEDED(hr) && SUCCEEDED(sc->GetDesc(&d));
    bool ours = false;
    {
        std::lock_guard lock(g_mutex);
        ours = static_cast<IUnknown*>(sc) == static_cast<IUnknown*>(g_swapchain);
    }
    const bool changed = have && ours && g_renderer_ready &&
                         (d.BufferCount != g_buffer_count || d.BufferDesc.Format != g_buffer_format);
    if (changed) g_reinit = true;
    log("overlay: %s(%u, %ux%u, format %d) on thread %lu -> 0x%08lX; real buffers %u, format %d%s", what, count, w, h,
        int(format), GetCurrentThreadId(), static_cast<unsigned long>(hr), have ? d.BufferCount : 0,
        have ? int(d.BufferDesc.Format) : -1, changed ? ": changed, panel re-init on next Present" : "");
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    before_resize(sc, count, format);
    const HRESULT hr = original_of(sc, 13, g_resize)(sc, count, w, h, format, flags);
    after_resize(sc, hr, "ResizeBuffers", count, w, h, format);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
                                     const UINT* masks, IUnknown* const* queues) {
    before_resize(sc, count, format);
    const HRESULT hr = original_of(sc, 39, g_resize1)(sc, count, w, h, format, flags, masks, queues);
    after_resize(sc, hr, "ResizeBuffers1", count, w, h, format);
    return hr;
}

// ---------------------------------------------------------------- swapchain capture

void on_swapchain(IUnknown* device, HWND hwnd, IUnknown* created) {
    ID3D12CommandQueue* queue = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        log("overlay: swapchain is not D3D12; panel unavailable");
        return;
    }
    D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
    IDXGISwapChain3* sc3 = nullptr;
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT || FAILED(created->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        queue->Release();
        log("overlay: unsupported swapchain/queue; panel unavailable");
        return;
    }
    void** vtable = *reinterpret_cast<void***>(sc3);
    patch_vtable(vtable, 8, reinterpret_cast<void*>(&hk_present), reinterpret_cast<void**>(&g_present));
    patch_vtable(vtable, 13, reinterpret_cast<void*>(&hk_resize), reinterpret_cast<void**>(&g_resize));
    patch_vtable(vtable, 22, reinterpret_cast<void*>(&hk_present1), reinterpret_cast<void**>(&g_present1));
    patch_vtable(vtable, 39, reinterpret_cast<void*>(&hk_resize1), reinterpret_cast<void**>(&g_resize1));
    {
        std::lock_guard lock(g_mutex);
        if (g_swapchain) g_reinit = true;  // replaced swapchain: old buffers are released on the render worker
        std::swap(g_swapchain, sc3);
        std::swap(g_queue, queue);
        g_hwnd = hwnd;
    }
    // Old references are released only after the render thread re-inits (they stay valid until then).
    log("overlay: captured D3D12 swapchain for window %p", static_cast<void*>(hwnd));
}

HRESULT STDMETHODCALLTYPE hk_create_swapchain(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
    const HRESULT hr = original_of(f, 10, g_create_swapchain)(f, device, desc, out);
    if (SUCCEEDED(hr) && out && *out && desc) on_swapchain(device, desc->OutputWindow, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_swapchain_hwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd,
                                                   const DXGI_SWAP_CHAIN_DESC1* desc,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                   IDXGISwapChain1** out) {
    const HRESULT hr = original_of(f, 15, g_create_swapchain_hwnd)(f, device, hwnd, desc, fs, output, out);
    if (SUCCEEDED(hr) && out && *out) on_swapchain(device, hwnd, *out);
    return hr;
}

void hook_factory(void* factory) {
    if (!factory) return;
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(static_cast<IUnknown*>(factory)->QueryInterface(IID_PPV_ARGS(&f2)))) return;
    void** vtable = *reinterpret_cast<void***>(f2);
    patch_vtable(vtable, 10, reinterpret_cast<void*>(&hk_create_swapchain), reinterpret_cast<void**>(&g_create_swapchain));
    patch_vtable(vtable, 15, reinterpret_cast<void*>(&hk_create_swapchain_hwnd),
                 reinterpret_cast<void**>(&g_create_swapchain_hwnd));
    f2->Release();
}

HRESULT WINAPI hk_create_factory(REFIID riid, void** out) {
    const HRESULT hr = g_create_factory(riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}

HRESULT WINAPI hk_create_factory1(REFIID riid, void** out) {
    const HRESULT hr = g_create_factory1(riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}

HRESULT WINAPI hk_create_factory2(UINT flags, REFIID riid, void** out) {
    const HRESULT hr = g_create_factory2(flags, riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}

// ---------------------------------------------------------------- window messages

bool is_mouse(UINT m) { return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT; }
bool is_keyboard(UINT m) { return (m >= WM_KEYFIRST && m <= WM_KEYLAST); }

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool menu_key = (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
                          g_menu_vk != 0 && wp == g_menu_vk;
    if (gl_overlay* overlay = g_overlay.load(); overlay && !menu_key) {
        const uint32_t cap = gl_overlay_win32_message(overlay, hwnd, msg, wp, lp);
        const bool alt_f4 = msg == WM_SYSKEYDOWN && wp == VK_F4;
        if (((cap & GL_OVERLAY_CAPTURE_MOUSE) && is_mouse(msg)) || ((cap & GL_OVERLAY_CAPTURE_KEYBOARD) && is_keyboard(msg) && !alt_f4))
            return 0;  // consumed by the panel (GyroLib already cleaned up WM_INPUT)
    }
    return CallWindowProcW(g_wndproc, hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- settings gate

bool panel_enabled_in_ini(const std::wstring& path, std::string& why) {
    std::ifstream in(path);
    if (!in) {
        why = "no gyrolib.ini yet (GyroLib defaults: F10 + Create/Options)";
        return true;
    }
    std::string line, key = "F10", pad = "1";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ui.menu_key=", 0) == 0) key = line.substr(12);
        else if (line.rfind("ui.gamepad_menu_shortcut=", 0) == 0) pad = line.substr(25);
    }
    why = "ui.menu_key=" + key + " ui.gamepad_menu_shortcut=" + pad;
    return !key.empty() || (pad != "0" && !pad.empty());
}

bool g_enabled = false;

// Protected exes (inline hook mode): hooking the dxgi.dll factory exports inline crashed Gears of War: E-Day,
// where Streamline, ReShade (as dxgi.dll) and RivaTuner already chain through those same exports. Instead,
// patch the vtable of the real System32 factory: every wrapper ends up calling it with the real queue and
// gets the real swapchain back, whose Present is hooked as usual. Runs on its own thread, which starts once
// the loader lock is released (no LoadLibrary/COM in DllMain) and well before the engine creates its RHI.
DWORD WINAPI hook_system_factory(void*) {
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    lstrcpynW(path + n, L"\\dxgi.dll", MAX_PATH - int(n));
    HMODULE dxgi = LoadLibraryW(path);
    using CreateFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto create = dxgi ? reinterpret_cast<CreateFactory2_t>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    IDXGIFactory2* factory = nullptr;
    const HRESULT hr = create ? create(0, IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
    if (FAILED(hr) || !factory) {
        log("overlay: system DXGI factory not created (0x%08lx); panel unavailable", static_cast<unsigned long>(hr));
        return 0;
    }
    hook_factory(factory);
    factory->Release();
    log("overlay: system DXGI factory vtable hooked (CreateSwapChain %s, CreateSwapChainForHwnd %s)",
        g_create_swapchain ? "ok" : "failed", g_create_swapchain_hwnd ? "ok" : "failed");
    return 0;
}

}  // namespace

void install(const std::wstring& settings_path) {
    std::string why;
    g_enabled = panel_enabled_in_ini(settings_path, why);
    if (!g_enabled) {
        log("overlay: disabled by gyrolib.ini (%s); no graphics hooks installed", why.c_str());
        return;
    }
    QueryPerformanceFrequency(&g_freq);
    if (hooks::inline_mode()) {
        if (HANDLE thread = CreateThread(nullptr, 0, &hook_system_factory, nullptr, 0, nullptr)) CloseHandle(thread);
        log("overlay: enabled (%s); protected exe: hooking the system DXGI factory vtable", why.c_str());
        return;
    }
    // Registered for every game module; in modular builds the RHI module loads later and is patched then.
    hooks::add("dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&hk_create_factory),
               reinterpret_cast<void**>(&g_create_factory));
    hooks::add("dxgi.dll", "CreateDXGIFactory1", reinterpret_cast<void*>(&hk_create_factory1),
               reinterpret_cast<void**>(&g_create_factory1));
    hooks::add("dxgi.dll", "CreateDXGIFactory2", reinterpret_cast<void*>(&hk_create_factory2),
               reinterpret_cast<void**>(&g_create_factory2));
    log("overlay: enabled (%s); DXGI factory hooks registered", why.c_str());
}

bool attach(gl_context* ctx) {
    if (!g_enabled) return false;
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (!g_swapchain) return false;
        hwnd = g_hwnd;
    }
    if (!g_overlay.load()) {
        gl_overlay* overlay = gl_overlay_create(ctx, GL_OVERLAY_ABI_VERSION);
        if (!overlay) {
            log("overlay: gl_overlay_create failed: %s", gl_overlay_error());
            g_enabled = false;
            return false;
        }
        g_overlay = overlay;
        log("overlay: settings panel attached");
    }
    if (!g_subclassed && hwnd) {
        g_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hk_wndproc)));
        g_subclassed = g_wndproc != nullptr;
        log("overlay: window messages %s", g_subclassed ? "forwarded" : "NOT forwarded");
    }
    return true;
}

void process() {
    if (gl_overlay* overlay = g_overlay.load()) gl_overlay_process(overlay);
}

bool available() { return g_overlay.load() != nullptr && !g_renderer_failed; }

void poll_menu_key(gl_context* ctx, bool focused) {
    gl_overlay* overlay = g_overlay.load();
    const uint32_t key = ctx ? gl_get_menu_key(ctx) : 0;  // F1..F24 as 1..24, 0 = disabled
    g_menu_vk = (key >= 1 && key <= 24) ? UINT(VK_F1 + key - 1) : 0;
    if (overlay && ctx && (gl_panel_open(ctx) != 0) != g_last_panel_open) {
        g_last_panel_open = gl_panel_open(ctx) != 0;
        log("overlay: panel state %s", g_last_panel_open ? "open" : "closed");
    }
    if (!overlay || !g_menu_vk) return;
    const bool down = focused && (GetAsyncKeyState(int(g_menu_vk.load())) & 0x8000) != 0;
    if (down && !g_menu_key_down) {
        const uint32_t open = gl_panel_open(ctx) ? 0 : 1;
        const int32_t r = g_renderer_failed ? GL_UNAVAILABLE : gl_overlay_set_open(overlay, open);
        log("overlay: F%u pressed: request %s (%d)%s%s", gl_get_menu_key(ctx), open ? "open" : "close", r,
            g_renderer_failed ? ", renderer failed" : "", g_renderer_ready ? "" : ", renderer not ready");
    }
    g_menu_key_down = down;
}

unsigned capture() {
    gl_overlay* overlay = g_overlay.load();
    return overlay ? gl_overlay_capture(overlay) : 0;
}

}  // namespace gyroue::overlay
