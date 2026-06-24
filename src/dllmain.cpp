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
#include "capture/dx11_resource_logger.h"

namespace core {
    Config& config() {
        static Config c;
        static bool loaded_env = false;
        if (!loaded_env) {
            loaded_env = true;
            wchar_t v[16]{};
            if (GetEnvironmentVariableW(L"FSRINJ_DX11_PACING", v, 16) > 0)
                c.dx11_frame_pacing.store(_wtoi(v) != 0);
            if (GetEnvironmentVariableW(L"FSRINJ_DX11_MENU_IN_GEN", v, 16) > 0)
                c.dx11_overlay_in_generated.store(_wtoi(v) != 0);
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
    capture::dx11log::install();              // RenderDoc-free DX11 resource/event logger
    hooks::install_swapchain_hooks();         // overlay + FSR present path
    depth::install();                         // generic depth-buffer extraction
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        core::log_init(dll_directory(self));
        core::log_line("[boot] fsr-injector starting");
        // Resolve the real DXGI now so forwarded exports never race the worker.
        proxy::load_real_dxgi();
        std::thread(install_hooks_thread).detach();
    } else if (reason == DLL_PROCESS_DETACH) {
        overlay::shutdown();
        depth::shutdown();
        capture::dx11log::shutdown();
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
// not by wrapping these.
// ---------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** pp) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    HRESULT hr = reinterpret_cast<Fn>(proxy::p_CreateDXGIFactory)(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) hooks::dx12::install_factory_hooks(reinterpret_cast<IUnknown*>(*pp));
    return hr;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** pp) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    HRESULT hr = reinterpret_cast<Fn>(proxy::p_CreateDXGIFactory1)(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) hooks::dx12::install_factory_hooks(reinterpret_cast<IUnknown*>(*pp));
    return hr;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    HRESULT hr = reinterpret_cast<Fn>(proxy::p_CreateDXGIFactory2)(flags, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) hooks::dx12::install_factory_hooks(reinterpret_cast<IUnknown*>(*pp));
    return hr;
}
__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pp) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    return reinterpret_cast<Fn>(proxy::p_DXGIGetDebugInterface1)(flags, riid, pp);
}
__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    using Fn = HRESULT(WINAPI*)();
    return reinterpret_cast<Fn>(proxy::p_DXGIDeclareAdapterRemovalSupport)();
}

} // extern "C"
