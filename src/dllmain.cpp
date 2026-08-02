#include <windows.h>
#include <thread>
#include <cstdlib>

#include "proxy/dxgi_proxy.h"
#include "hooks/swapchain_hook.h"
#include "hooks/depth_hook.h"
#include "hooks/dx12_queue_capture.h"
#include "overlay/overlay.h"
#include "core/config.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/version.h"

namespace core {
    Config& config() {
        static Config c;
        static bool loaded = false;
        if (!loaded) {
            loaded = true;
            // settings:: resolves env var first, then fsrinj.ini, then fallback.
            c.dx11_frame_pacing.store(settings::get_bool(L"FSRINJ_DX11_PACING", true));
            c.dx11_overlay_in_generated.store(settings::get_bool(L"FSRINJ_DX11_MENU_IN_GEN", true));
            c.sharpness.store(settings::get_float(L"FSRINJ_SHARPNESS", 0.5f, 0.0f, 1.0f));
            c.toggle_key.store(settings::get_int(L"FSRINJ_KEY_MENU", 0x24 /*VK_HOME*/, 1, 254));
        }
        return c;
    }
}

static std::wstring dll_directory(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    auto pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

static void install_hooks_thread() {
    // Hooking touches other modules, so keep it off the loader lock.
    hooks::install_swapchain_hooks();         // overlay + FSR present path
    depth::install();                         // generic depth-buffer extraction
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        const std::wstring dir = dll_directory(self);
        core::log_init(dir);
        core::settings::init(dir);
        core::log_line("[boot] fsr-injector starting, build " FSRINJ_VERSION);
        // Resolve the real DXGI now so forwarded exports never race the worker.
        proxy::load_real_dxgi();
        std::thread(install_hooks_thread).detach();
    } else if (reason == DLL_PROCESS_DETACH) {
        // 'reserved' != nullptr means the PROCESS IS TERMINATING: other threads
        // have already been killed (possibly mid-lock), so touching MinHook, the
        // GPU objects, or other DLLs from here is a classic deadlock/crash.
        // The OS reclaims everything anyway -- just flush the log and get out.
        // Full cleanup only runs on an explicit dynamic FreeLibrary, which for
        // a dxgi proxy essentially never happens.
        if (reserved != nullptr) {
            core::log_line("[boot] process terminating; skipping unsafe cleanup");
            core::log_shutdown();
            return TRUE;
        }
        overlay::shutdown();
        depth::shutdown();
        hooks::dx12::shutdown();
        hooks::remove_swapchain_hooks();
        proxy::unload_real_dxgi();
        core::log_shutdown();
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Forwarded dxgi.dll exports. Each just trampolines into the real library so the
// game runs identically. Frame interception is done by the Present vtable hook,
// not by wrapping these. If the real dxgi.dll somehow failed to resolve, we
// retry once lazily and otherwise fail gracefully instead of calling null.
// ---------------------------------------------------------------------------
namespace {
    FARPROC ensure(FARPROC& p) {
        if (!p) proxy::load_real_dxgi();
        return p;
    }
}

extern "C" {

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** pp) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    Fn fn = reinterpret_cast<Fn>(ensure(proxy::p_CreateDXGIFactory));
    if (!fn) return E_NOINTERFACE;
    HRESULT hr = fn(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) hooks::dx12::install_factory_hooks(reinterpret_cast<IUnknown*>(*pp));
    return hr;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** pp) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    Fn fn = reinterpret_cast<Fn>(ensure(proxy::p_CreateDXGIFactory1));
    if (!fn) return E_NOINTERFACE;
    HRESULT hr = fn(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) hooks::dx12::install_factory_hooks(reinterpret_cast<IUnknown*>(*pp));
    return hr;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    Fn fn = reinterpret_cast<Fn>(ensure(proxy::p_CreateDXGIFactory2));
    if (!fn) return E_NOINTERFACE;
    HRESULT hr = fn(flags, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) hooks::dx12::install_factory_hooks(reinterpret_cast<IUnknown*>(*pp));
    return hr;
}
__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pp) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    Fn fn = reinterpret_cast<Fn>(ensure(proxy::p_DXGIGetDebugInterface1));
    if (!fn) return E_NOINTERFACE;
    return fn(flags, riid, pp);
}
__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    using Fn = HRESULT(WINAPI*)();
    Fn fn = reinterpret_cast<Fn>(ensure(proxy::p_DXGIDeclareAdapterRemovalSupport));
    if (!fn) return E_NOINTERFACE;
    return fn();
}

} // extern "C"
