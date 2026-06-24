#include "hooks/swapchain_hook.h"
#include "capture/dx11_resource_logger.h"
#include "overlay/overlay.h"
#include "fsr/framegen.h"
#include "fsr/upscaler.h"
#include "core/log.h"
#include "core/config.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>     // IDXGISwapChain1 / Present1
#include <MinHook.h>

#pragma comment(lib, "d3d11.lib")

namespace hooks {

using PresentFn       = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn      = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn       g_orig_present  = nullptr;
static Present1Fn      g_orig_present1 = nullptr;
static ResizeBuffersFn g_orig_resize   = nullptr;

static bool is_d3d11_swapchain(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) {
        dev->Release();
        return true;
    }
    return false;
}

// Classic present path (DISCARD / older games).
static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    capture::dx11log::note_swapchain_present(sc);
    const bool d3d11 = is_d3d11_swapchain(sc);
    if (d3d11) {
        upscaler::sharpen(sc);
        if (core::config().dx11_overlay_in_generated.load()) {
            // Render UI before framegen capture so generated frames also carry the menu.
            // This avoids the alternating-menu flicker seen in DX11 titles.
            overlay::on_present(sc);
            framegen::before_present(sc, reinterpret_cast<framegen::PresentTrampoline>(g_orig_present), flags);
        } else {
            framegen::before_present(sc, reinterpret_cast<framegen::PresentTrampoline>(g_orig_present), flags);
            overlay::on_present(sc);
        }
    } else {
        overlay::on_present(sc);
    }
    HRESULT hr = g_orig_present(sc, sync, flags);
    if (SUCCEEDED(hr)) overlay::after_present(sc, flags, g_orig_present);
    return hr;
}

// Flip-model present path (most modern D3D11 games).
static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS* pp) {
    capture::dx11log::note_swapchain_present(sc);
    const bool d3d11 = is_d3d11_swapchain(sc);
    if (d3d11) {
        upscaler::sharpen(sc);
        if (core::config().dx11_overlay_in_generated.load()) {
            overlay::on_present(sc);
            framegen::before_present(sc, reinterpret_cast<framegen::PresentTrampoline>(g_orig_present), flags);
        } else {
            framegen::before_present(sc, reinterpret_cast<framegen::PresentTrampoline>(g_orig_present), flags);
            overlay::on_present(sc);
        }
    } else {
        overlay::on_present(sc);
    }
    HRESULT hr = g_orig_present1(sc, sync, flags, pp);
    if (SUCCEEDED(hr)) overlay::after_present1(sc, flags, pp, g_orig_present1);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w,
                                                  UINT h, DXGI_FORMAT fmt, UINT flags) {
    overlay::on_resize_buffers();
    HRESULT hr = g_orig_resize(sc, count, w, h, fmt, flags);
    overlay::on_after_resize(sc);
    return hr;
}

// Spin up a hidden swapchain to read the vtable(s). vt = IDXGISwapChain vtable,
// vt1 = IDXGISwapChain1 vtable (null if the runtime is too old to support it).
static bool acquire_vtables(void**& vt, void**& vt1) {
    vt = vt1 = nullptr;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"fsrinj_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                              0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hwnd;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain*      sc  = nullptr;
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL    fl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);

    bool ok = SUCCEEDED(hr) && sc;
    if (ok) {
        vt = *reinterpret_cast<void***>(sc);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            vt1 = *reinterpret_cast<void***>(sc1);   // same object, extended vtable
            sc1->Release();
        }
    }

    if (sc)  sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

bool install_swapchain_hooks() {
    if (MH_Initialize() != MH_OK) { LOGF("[hook] MH_Initialize failed"); return false; }

    void** vt = nullptr; void** vt1 = nullptr;
    if (!acquire_vtables(vt, vt1)) { LOGF("[hook] could not acquire swapchain vtable"); return false; }

    // IDXGISwapChain:  8 = Present, 13 = ResizeBuffers
    // IDXGISwapChain1: 22 = Present1
    if (MH_CreateHook(vt[8], reinterpret_cast<LPVOID>(&hk_Present),
                      reinterpret_cast<void**>(&g_orig_present)) != MH_OK) return false;
    if (MH_CreateHook(vt[13], reinterpret_cast<LPVOID>(&hk_ResizeBuffers),
                      reinterpret_cast<void**>(&g_orig_resize)) != MH_OK) return false;

    if (vt1) {
        if (MH_CreateHook(vt1[22], reinterpret_cast<LPVOID>(&hk_Present1),
                          reinterpret_cast<void**>(&g_orig_present1)) != MH_OK)
            LOGF("[hook] Present1 hook create failed (continuing with Present only)");
        else
            LOGF("[hook] Present1 path available");
    } else {
        LOGF("[hook] IDXGISwapChain1 not available; Present-only");
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { LOGF("[hook] enable failed"); return false; }
    LOGF("[hook] Present + Present1 + ResizeBuffers hooked");
    return true;
}

void remove_swapchain_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace hooks
