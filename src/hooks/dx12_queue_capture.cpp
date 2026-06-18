#include "hooks/dx12_queue_capture.h"
#include "core/log.h"

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d12.h>
#include <MinHook.h>

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace hooks::dx12 {
namespace {
    using CreateSwapChainFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    using CreateSwapChainForHwndFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    using CreateSwapChainForCoreWindowFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    using CreateSwapChainForCompositionFn = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);

    CreateSwapChainFn               g_orig_create_swapchain = nullptr;
    CreateSwapChainForHwndFn        g_orig_create_for_hwnd = nullptr;
    CreateSwapChainForCoreWindowFn  g_orig_create_for_core = nullptr;
    CreateSwapChainForCompositionFn g_orig_create_for_composition = nullptr;

    std::mutex g_mtx;
    std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> g_queues;
    std::unordered_set<void*> g_hooked_targets;

    bool ensure_min_hook() {
        MH_STATUS s = MH_Initialize();
        return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED;
    }

    void remember_queue(IUnknown* maybe_queue, IDXGISwapChain* sc) {
        if (!maybe_queue || !sc) return;

        ID3D12CommandQueue* q = nullptr;
        if (FAILED(maybe_queue->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&q))) || !q)
            return; // D3D11 path passes a device here, not a D3D12 queue.

        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_queues.find(sc);
        if (it != g_queues.end() && it->second) it->second->Release();
        g_queues[sc] = q; // keep the AddRef from QueryInterface
        LOGF("[dx12] captured ID3D12CommandQueue %p for swapchain %p", static_cast<void*>(q), static_cast<void*>(sc));
    }

    HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* factory, IUnknown* device,
                                                 DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc) {
        HRESULT hr = g_orig_create_swapchain(factory, device, desc, sc);
        if (SUCCEEDED(hr) && sc && *sc) remember_queue(device, *sc);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
                                                        const DXGI_SWAP_CHAIN_DESC1* desc,
                                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                        IDXGIOutput* restrict_to,
                                                        IDXGISwapChain1** sc) {
        HRESULT hr = g_orig_create_for_hwnd(factory, device, hwnd, desc, fs, restrict_to, sc);
        if (SUCCEEDED(hr) && sc && *sc) remember_queue(device, *sc);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device,
                                                              IUnknown* window,
                                                              const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              IDXGIOutput* restrict_to,
                                                              IDXGISwapChain1** sc) {
        HRESULT hr = g_orig_create_for_core(factory, device, window, desc, restrict_to, sc);
        if (SUCCEEDED(hr) && sc && *sc) remember_queue(device, *sc);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device,
                                                               const DXGI_SWAP_CHAIN_DESC1* desc,
                                                               IDXGIOutput* restrict_to,
                                                               IDXGISwapChain1** sc) {
        HRESULT hr = g_orig_create_for_composition(factory, device, desc, restrict_to, sc);
        if (SUCCEEDED(hr) && sc && *sc) remember_queue(device, *sc);
        return hr;
    }

    template <class Fn>
    void hook_once(void* target, void* detour, Fn* original, const char* name) {
        if (!target) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_hooked_targets.contains(target)) return;
        if (MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK) {
            g_hooked_targets.insert(target);
            MH_EnableHook(target);
            LOGF("[dx12] hooked %s", name);
        }
    }
}

bool install_factory_hooks(IUnknown* factory_unknown) {
    if (!factory_unknown || !ensure_min_hook()) return false;

    IDXGIFactory* f0 = nullptr;
    if (SUCCEEDED(factory_unknown->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&f0))) && f0) {
        void** vt = *reinterpret_cast<void***>(f0);
        // IDXGIFactory::CreateSwapChain = vtable slot 10.
        hook_once(vt[10], reinterpret_cast<void*>(&hk_CreateSwapChain), &g_orig_create_swapchain, "IDXGIFactory::CreateSwapChain");
        f0->Release();
    }

    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(factory_unknown->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2))) && f2) {
        void** vt = *reinterpret_cast<void***>(f2);
        // IDXGIFactory2 slots: 15 = ForHwnd, 16 = ForCoreWindow.
        hook_once(vt[15], reinterpret_cast<void*>(&hk_CreateSwapChainForHwnd), &g_orig_create_for_hwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
        hook_once(vt[16], reinterpret_cast<void*>(&hk_CreateSwapChainForCoreWindow), &g_orig_create_for_core, "IDXGIFactory2::CreateSwapChainForCoreWindow");
        f2->Release();
    }

    IDXGIFactory2* f2_comp = nullptr;
    if (SUCCEEDED(factory_unknown->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2_comp))) && f2_comp) {
        void** vt = *reinterpret_cast<void***>(f2_comp);
        // IDXGIFactory2::CreateSwapChainForComposition = slot 24.
        hook_once(vt[24], reinterpret_cast<void*>(&hk_CreateSwapChainForComposition), &g_orig_create_for_composition, "IDXGIFactory2::CreateSwapChainForComposition");
        f2_comp->Release();
    }

    return true;
}

ID3D12CommandQueue* queue_for_swapchain(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_queues.find(sc);
    if (it == g_queues.end() || !it->second) return nullptr;
    it->second->AddRef();
    return it->second;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& kv : g_queues) if (kv.second) kv.second->Release();
    g_queues.clear();
    g_hooked_targets.clear();
}

} // namespace hooks::dx12
