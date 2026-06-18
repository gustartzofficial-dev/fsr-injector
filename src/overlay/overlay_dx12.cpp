#include "overlay/overlay_dx12.h"
#include "hooks/dx12_queue_capture.h"
#include "core/config.h"
#include "core/log.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <vector>

namespace overlay::dx12 {
namespace {
    struct FrameContext {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12Resource* backbuffer = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        UINT64 fence_value = 0;
    };

    bool g_init = false;
    IDXGISwapChain3* g_sc3 = nullptr;
    ID3D12Device* g_dev = nullptr;
    ID3D12CommandQueue* g_queue = nullptr;
    ID3D12DescriptorHeap* g_rtv_heap = nullptr;
    ID3D12GraphicsCommandList* g_cmd = nullptr;
    ID3D12Fence* g_fence = nullptr;
    HANDLE g_fence_event = nullptr;
    UINT64 g_next_fence_value = 1;
    std::vector<FrameContext> g_frames;
    UINT g_rtv_stride = 0;
    DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    HWND g_hwnd = nullptr;
    bool g_marker_enabled = true;
    unsigned g_present_count = 0;
    const unsigned kWarmupPresents = 3;

    bool env_disabled(const wchar_t* name) {
        wchar_t value[16]{};
        DWORD n = GetEnvironmentVariableW(name, value, 16);
        if (n == 0 || n >= 16) return false;
        return value[0] == L'0' || value[0] == L'n' || value[0] == L'N' ||
               value[0] == L'f' || value[0] == L'F';
    }

    bool wait_for_fence(UINT64 value) {
        if (!g_fence || value == 0) return true;
        if (g_fence->GetCompletedValue() >= value) return true;
        HRESULT hr = g_fence->SetEventOnCompletion(value, g_fence_event);
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] SetEventOnCompletion failed hr=0x%08lX", hr);
            return false;
        }
        WaitForSingleObject(g_fence_event, 2000);
        return g_fence->GetCompletedValue() >= value;
    }

    bool wait_for_frame(FrameContext& f) {
        if (!wait_for_fence(f.fence_value)) {
            LOGF("[overlay-dx12] fence wait timed out; skipping marker frame");
            return false;
        }
        f.fence_value = 0;
        return true;
    }

    void signal_frame(FrameContext& f) {
        if (!g_queue || !g_fence) return;
        const UINT64 value = g_next_fence_value++;
        HRESULT hr = g_queue->Signal(g_fence, value);
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] queue Signal failed hr=0x%08lX", hr);
            return;
        }
        f.fence_value = value;
    }

    void wait_for_gpu_idle() {
        if (!g_queue || !g_fence) return;
        const UINT64 value = g_next_fence_value++;
        if (SUCCEEDED(g_queue->Signal(g_fence, value))) wait_for_fence(value);
        for (auto& f : g_frames) f.fence_value = 0;
    }

    void release_frame_resources() {
        wait_for_gpu_idle();
        for (auto& f : g_frames) {
            if (f.backbuffer) { f.backbuffer->Release(); f.backbuffer = nullptr; }
            if (f.allocator) { f.allocator->Release(); f.allocator = nullptr; }
            f.fence_value = 0;
        }
        g_frames.clear();
        if (g_cmd) { g_cmd->Release(); g_cmd = nullptr; }
        if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
    }

    bool create_render_targets(IDXGISwapChain* sc) {
        DXGI_SWAP_CHAIN_DESC desc{};
        HRESULT hr = sc->GetDesc(&desc);
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] GetDesc failed hr=0x%08lX", hr);
            return false;
        }
        g_format = desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : desc.BufferDesc.Format;
        const UINT buffer_count = desc.BufferCount ? desc.BufferCount : 2;

        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors = buffer_count;
        hr = g_dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_rtv_heap));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateDescriptorHeap(RTV) failed hr=0x%08lX", hr);
            return false;
        }
        g_rtv_stride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        g_frames.resize(buffer_count);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < buffer_count; ++i) {
            g_frames[i].rtv = cpu;
            hr = sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].backbuffer));
            if (FAILED(hr)) {
                LOGF("[overlay-dx12] GetBuffer(%u) failed hr=0x%08lX", i, hr);
                return false;
            }
            g_dev->CreateRenderTargetView(g_frames[i].backbuffer, nullptr, cpu);
            hr = g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator));
            if (FAILED(hr)) {
                LOGF("[overlay-dx12] CreateCommandAllocator(%u) failed hr=0x%08lX", i, hr);
                return false;
            }
            cpu.ptr += g_rtv_stride;
        }

        hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr, IID_PPV_ARGS(&g_cmd));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateCommandList failed hr=0x%08lX", hr);
            return false;
        }
        g_cmd->Close();
        return true;
    }

    bool init(IDXGISwapChain* sc) {
        HRESULT hr = sc->QueryInterface(IID_PPV_ARGS(&g_sc3));
        if (FAILED(hr) || !g_sc3) {
            LOGF("[overlay-dx12] QueryInterface(IDXGISwapChain3) failed hr=0x%08lX", hr);
            return false;
        }
        hr = sc->GetDevice(IID_PPV_ARGS(&g_dev));
        if (FAILED(hr) || !g_dev) {
            LOGF("[overlay-dx12] GetDevice(ID3D12Device) failed hr=0x%08lX", hr);
            return false;
        }

        g_queue = hooks::dx12::queue_for_swapchain(sc);
        if (!g_queue) {
            LOGF("[overlay-dx12] no captured ID3D12CommandQueue for swapchain %p", static_cast<void*>(sc));
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;
        g_marker_enabled = !env_disabled(L"FSRINJ_DX12_MARKER");

        hr = g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateFence failed hr=0x%08lX", hr);
            return false;
        }
        g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_fence_event) {
            LOGF("[overlay-dx12] CreateEvent failed err=%lu", GetLastError());
            return false;
        }

        if (!create_render_targets(sc)) return false;

        LOGF("[overlay-dx12] native marker initialized on hwnd %p buffers=%u format=%u queue=%p marker=%s",
             static_cast<void*>(g_hwnd), (unsigned)g_frames.size(), (unsigned)g_format,
             static_cast<void*>(g_queue), g_marker_enabled ? "on" : "off");
        LOGF("[overlay-dx12] ImGui is intentionally bypassed on DX12; this patch tests raw D3D12 backbuffer writes only");
        return true;
    }

    void render_marker_rect(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
        // Solid magenta diagnostic marker. Uses ClearRenderTargetView with a small rect so we do
        // not need an ImGui context, shaders, descriptor heaps, root signatures, or a PSO.
        const FLOAT color[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
        const D3D12_RECT rect = { 16, 16, 96, 96 };
        cmd->ClearRenderTargetView(rtv, color, 1, &rect);
    }
}

bool on_present(IDXGISwapChain* sc) {
    if (!g_init) {
        if (!init(sc)) return false;
        g_init = true;
        g_present_count = 0;
        LOGF("[overlay-dx12] init-only present skipped; marker render begins after warmup");
        return true;
    }

    ++g_present_count;
    if (g_marker_enabled && g_present_count <= kWarmupPresents) {
        LOGF("[overlay-dx12] warmup present %u/%u; skipping marker", g_present_count, kWarmupPresents);
        return true;
    }

    static bool prev = false;
    bool down = (GetAsyncKeyState(core::config().toggle_key.load()) & 0x8000) != 0;
    if (down && !prev) {
        bool v = core::config().overlay_visible.load();
        core::config().overlay_visible.store(!v);
    }
    prev = down;

    if (!g_marker_enabled) return true;
    if (!core::config().overlay_visible.load()) return true;

    LOGF("[overlay-dx12] marker render begin present=%u", g_present_count);

    const UINT idx = g_sc3 ? g_sc3->GetCurrentBackBufferIndex() : 0;
    if (idx >= g_frames.size()) {
        LOGF("[overlay-dx12] invalid backbuffer index %u size=%u", idx, (unsigned)g_frames.size());
        return true;
    }
    FrameContext& f = g_frames[idx];
    if (!f.allocator || !f.backbuffer || !g_cmd || !g_queue) return true;
    if (!wait_for_frame(f)) return true;

    LOGF("[overlay-dx12] step: allocator reset idx=%u", idx);
    HRESULT hr = f.allocator->Reset();
    if (FAILED(hr)) {
        LOGF("[overlay-dx12] allocator Reset failed hr=0x%08lX", hr);
        return true;
    }
    LOGF("[overlay-dx12] step: command list reset");
    hr = g_cmd->Reset(f.allocator, nullptr);
    if (FAILED(hr)) {
        LOGF("[overlay-dx12] command list Reset failed hr=0x%08lX", hr);
        return true;
    }

    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = f.backbuffer;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    LOGF("[overlay-dx12] step: barrier present->rt");
    g_cmd->ResourceBarrier(1, &b1);

    LOGF("[overlay-dx12] step: native marker ClearRenderTargetView");
    render_marker_rect(g_cmd, f.rtv);

    D3D12_RESOURCE_BARRIER b2 = b1;
    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    LOGF("[overlay-dx12] step: barrier rt->present");
    g_cmd->ResourceBarrier(1, &b2);

    LOGF("[overlay-dx12] step: command list close");
    hr = g_cmd->Close();
    if (FAILED(hr)) {
        LOGF("[overlay-dx12] command list Close failed hr=0x%08lX", hr);
        return true;
    }

    ID3D12CommandList* lists[] = { g_cmd };
    LOGF("[overlay-dx12] step: ExecuteCommandLists");
    g_queue->ExecuteCommandLists(1, lists);
    LOGF("[overlay-dx12] step: Signal fence");
    signal_frame(f);
    LOGF("[overlay-dx12] marker render end");
    return true;
}

void on_resize_buffers() { release_frame_resources(); }
void on_after_resize(IDXGISwapChain* sc) {
    if (g_init && g_dev) {
        if (!create_render_targets(sc)) LOGF("[overlay-dx12] recreate render targets failed after ResizeBuffers");
    }
}

void shutdown() {
    if (g_init) wait_for_gpu_idle();
    release_frame_resources();
    if (g_fence_event) { CloseHandle(g_fence_event); g_fence_event = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_queue) { g_queue->Release(); g_queue = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
    g_next_fence_value = 1;
    g_marker_enabled = true;
    g_present_count = 0;
    g_init = false;
}

} // namespace overlay::dx12
