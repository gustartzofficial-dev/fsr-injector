#include "hooks/dx12_queue_capture.h"
#include "core/log.h"
#include "capture/generic_resource_scout.h"

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


    using ExecuteCommandListsFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using CreateRenderTargetViewFn = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    using CreateDepthStencilViewFn = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    using CreateCommandListFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
    using ResourceBarrierFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
    using DrawInstancedFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
    using DrawIndexedInstancedFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
    using SetPipelineStateFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    using SetGraphicsRootDescriptorTableFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);

    ExecuteCommandListsFn g_orig_execute_command_lists = nullptr;
    CreateRenderTargetViewFn g_orig_create_rtv = nullptr;
    CreateDepthStencilViewFn g_orig_create_dsv = nullptr;
    CreateCommandListFn g_orig_create_command_list = nullptr;
    ResourceBarrierFn g_orig_resource_barrier = nullptr;
    OMSetRenderTargetsFn g_orig_omset_render_targets = nullptr;
    DrawInstancedFn g_orig_draw_instanced = nullptr;
    DrawIndexedInstancedFn g_orig_draw_indexed_instanced = nullptr;
    SetPipelineStateFn g_orig_set_pipeline_state = nullptr;
    SetGraphicsRootDescriptorTableFn g_orig_set_graphics_root_descriptor_table = nullptr;

    std::mutex g_mtx;
    std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> g_queues;
    std::unordered_set<void*> g_hooked_targets;

    bool ensure_min_hook() {
        MH_STATUS s = MH_Initialize();
        return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED;
    }

    template <class Fn>
    void hook_once(void* target, void* detour, Fn* original, const char* name) {
        if (!target) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_hooked_targets.contains(target)) return;
        MH_STATUS create_status = MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
        if (create_status == MH_OK) {
            g_hooked_targets.insert(target);
            MH_STATUS enable_status = MH_EnableHook(target);
            if (enable_status == MH_OK || enable_status == MH_ERROR_ENABLED) {
                LOGF("[dx12] hooked %s", name);
            } else {
                LOGF("[dx12] failed enabling %s mh=%d", name, (int)enable_status);
            }
        } else if (create_status != MH_ERROR_ALREADY_CREATED) {
            LOGF("[dx12] failed hooking %s mh=%d", name, (int)create_status);
        }
    }



    void STDMETHODCALLTYPE hk_CreateRenderTargetView(ID3D12Device* device, ID3D12Resource* resource,
                                                     const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        capture::scout::note_dx12_rtv_descriptor(handle, resource, desc);
        g_orig_create_rtv(device, resource, desc, handle);
    }

    void STDMETHODCALLTYPE hk_CreateDepthStencilView(ID3D12Device* device, ID3D12Resource* resource,
                                                     const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        capture::scout::note_dx12_dsv_descriptor(handle, resource, desc);
        g_orig_create_dsv(device, resource, desc, handle);
    }

    void STDMETHODCALLTYPE hk_ResourceBarrier(ID3D12GraphicsCommandList* list, UINT count,
                                              const D3D12_RESOURCE_BARRIER* barriers) {
        capture::scout::note_dx12_resource_barrier(count, barriers);
        g_orig_resource_barrier(list, count, barriers);
    }

    void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT num_rt,
                                                 const D3D12_CPU_DESCRIPTOR_HANDLE* rt_handles,
                                                 BOOL single_handle,
                                                 const D3D12_CPU_DESCRIPTOR_HANDLE* dsv_handle) {
        capture::scout::note_dx12_omset(num_rt, rt_handles, dsv_handle);
        g_orig_omset_render_targets(list, num_rt, rt_handles, single_handle, dsv_handle);
    }

    void STDMETHODCALLTYPE hk_DrawInstanced(ID3D12GraphicsCommandList* list, UINT vertex_count, UINT instance_count,
                                           UINT start_vertex, UINT start_instance) {
        capture::scout::note_dx12_draw_call(false);
        g_orig_draw_instanced(list, vertex_count, instance_count, start_vertex, start_instance);
    }

    void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D12GraphicsCommandList* list, UINT index_count, UINT instance_count,
                                                  UINT start_index, INT base_vertex, UINT start_instance) {
        capture::scout::note_dx12_draw_call(true);
        g_orig_draw_indexed_instanced(list, index_count, instance_count, start_index, base_vertex, start_instance);
    }

    void STDMETHODCALLTYPE hk_SetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
        capture::scout::note_dx12_set_pipeline_state();
        g_orig_set_pipeline_state(list, pso);
    }

    void STDMETHODCALLTYPE hk_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* list, UINT root_parameter_index,
                                                             D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) {
        capture::scout::note_dx12_set_graphics_root_descriptor_table();
        g_orig_set_graphics_root_descriptor_table(list, root_parameter_index, base_descriptor);
    }

    void hook_graphics_command_list(ID3D12GraphicsCommandList* list);

    HRESULT STDMETHODCALLTYPE hk_CreateCommandList(ID3D12Device* device, UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                                   ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial_state,
                                                   REFIID riid, void** command_list) {
        HRESULT hr = g_orig_create_command_list(device, node_mask, type, allocator, initial_state, riid, command_list);
        if (SUCCEEDED(hr) && command_list && *command_list && type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            ID3D12GraphicsCommandList* gl = nullptr;
            IUnknown* unk = static_cast<IUnknown*>(*command_list);
            if (SUCCEEDED(unk->QueryInterface(__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&gl))) && gl) {
                hook_graphics_command_list(gl);
                gl->Release();
            }
        }
        return hr;
    }

    void hook_device_views(ID3D12Device* device) {
        if (!device) return;
        void** vt = *reinterpret_cast<void***>(device);
        // ID3D12Device vtable slots: 20=CreateRenderTargetView, 21=CreateDepthStencilView.
        hook_once(vt[20], reinterpret_cast<void*>(&hk_CreateRenderTargetView), &g_orig_create_rtv, "ID3D12Device::CreateRenderTargetView");
        hook_once(vt[21], reinterpret_cast<void*>(&hk_CreateDepthStencilView), &g_orig_create_dsv, "ID3D12Device::CreateDepthStencilView");
        hook_once(vt[12], reinterpret_cast<void*>(&hk_CreateCommandList), &g_orig_create_command_list, "ID3D12Device::CreateCommandList");
    }

    void hook_graphics_command_list(ID3D12GraphicsCommandList* list) {
        if (!list) return;
        capture::scout::note_dx12_command_list_seen();
        void** vt = *reinterpret_cast<void***>(list);
        // ID3D12GraphicsCommandList vtable slots include inherited IUnknown/Object/CommandList methods.
        // 12=DrawInstanced, 13=DrawIndexedInstanced, 25=SetPipelineState,
        // 26=ResourceBarrier, 32=SetGraphicsRootDescriptorTable, 46=OMSetRenderTargets.
        hook_once(vt[12], reinterpret_cast<void*>(&hk_DrawInstanced), &g_orig_draw_instanced, "ID3D12GraphicsCommandList::DrawInstanced");
        hook_once(vt[13], reinterpret_cast<void*>(&hk_DrawIndexedInstanced), &g_orig_draw_indexed_instanced, "ID3D12GraphicsCommandList::DrawIndexedInstanced");
        hook_once(vt[25], reinterpret_cast<void*>(&hk_SetPipelineState), &g_orig_set_pipeline_state, "ID3D12GraphicsCommandList::SetPipelineState");
        hook_once(vt[26], reinterpret_cast<void*>(&hk_ResourceBarrier), &g_orig_resource_barrier, "ID3D12GraphicsCommandList::ResourceBarrier");
        hook_once(vt[32], reinterpret_cast<void*>(&hk_SetGraphicsRootDescriptorTable), &g_orig_set_graphics_root_descriptor_table, "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable");
        hook_once(vt[46], reinterpret_cast<void*>(&hk_OMSetRenderTargets), &g_orig_omset_render_targets, "ID3D12GraphicsCommandList::OMSetRenderTargets");
    }

    void STDMETHODCALLTYPE hk_ExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
        capture::scout::note_dx12_execute_call(count);
        if (lists) {
            for (UINT i = 0; i < count; ++i) {
                ID3D12GraphicsCommandList* gl = nullptr;
                if (lists[i] && SUCCEEDED(lists[i]->QueryInterface(__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&gl))) && gl) {
                    hook_graphics_command_list(gl);
                    gl->Release();
                }
            }
        }
        capture::scout::log_dx12_candidates_periodic();
        g_orig_execute_command_lists(queue, count, lists);
    }

    void hook_queue_execute(ID3D12CommandQueue* queue) {
        if (!queue) return;
        void** vt = *reinterpret_cast<void***>(queue);
        // ID3D12CommandQueue::ExecuteCommandLists = slot 8.
        hook_once(vt[8], reinterpret_cast<void*>(&hk_ExecuteCommandLists), &g_orig_execute_command_lists, "ID3D12CommandQueue::ExecuteCommandLists");
    }

    void remember_queue(IUnknown* maybe_queue, IDXGISwapChain* sc) {
        if (!maybe_queue || !sc) return;

        ID3D12CommandQueue* q = nullptr;
        if (FAILED(maybe_queue->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&q))) || !q)
            return; // D3D11 path passes a device here, not a D3D12 queue.

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_queues.find(sc);
            if (it != g_queues.end() && it->second) it->second->Release();
            g_queues[sc] = q; // keep the AddRef from QueryInterface
        }
        LOGF("[dx12] captured ID3D12CommandQueue %p for swapchain %p", static_cast<void*>(q), static_cast<void*>(sc));

        hook_queue_execute(q);
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) && dev) {
            hook_device_views(dev);
            dev->Release();
        }
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
