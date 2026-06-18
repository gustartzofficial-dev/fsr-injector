#include "overlay/overlay_dx12.h"
#include "hooks/dx12_queue_capture.h"
#include "core/config.h"
#include "core/log.h"
#include "fsr/framegen.h"
#include "detect/upscaler_detect.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay::dx12 {
namespace {
    struct FrameContext {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12Resource* backbuffer = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    };

    bool g_init = false;
    IDXGISwapChain3* g_sc3 = nullptr;
    ID3D12Device* g_dev = nullptr;
    ID3D12CommandQueue* g_queue = nullptr;
    ID3D12DescriptorHeap* g_rtv_heap = nullptr;
    ID3D12DescriptorHeap* g_srv_heap = nullptr;
    ID3D12GraphicsCommandList* g_cmd = nullptr;
    std::vector<FrameContext> g_frames;
    UINT g_rtv_stride = 0;
    DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    HWND g_hwnd = nullptr;
    WNDPROC g_orig_wndproc = nullptr;
    detect::DetectResult g_profile;
    bool g_profile_done = false;
    bool g_render_enabled = false;

    bool env_enabled(const wchar_t* name) {
        wchar_t value[16]{};
        DWORD n = GetEnvironmentVariableW(name, value, 16);
        if (n == 0 || n >= 16) return false;
        return value[0] == L'1' || value[0] == L'y' || value[0] == L'Y' ||
               value[0] == L't' || value[0] == L'T';
    }

    LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (core::config().overlay_visible.load() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return true;
        return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
    }

    void release_frame_resources() {
        for (auto& f : g_frames) {
            if (f.backbuffer) { f.backbuffer->Release(); f.backbuffer = nullptr; }
            if (f.allocator) { f.allocator->Release(); f.allocator = nullptr; }
        }
        g_frames.clear();
        if (g_cmd) { g_cmd->Release(); g_cmd = nullptr; }
        if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
    }

    void draw_menu() {
        auto& cfg = core::config();
        ImGui::Begin("FSR Injector - DX12");

        if (!g_profile_done) { g_profile = detect::scan_loaded_modules(); g_profile_done = true; }
        ImGui::Text("Game profile: %s", detect::profile_name(g_profile.profile));
        ImGui::Separator();

        bool up = cfg.upscaling_enabled.load();
        if (ImGui::Checkbox("Enable adaptive sharpening", &up)) cfg.upscaling_enabled.store(up);

        const char* modes[] = { "Off", "Quality", "Balanced", "Performance", "Ultra Performance" };
        int mode = cfg.upscaler_mode.load();
        if (ImGui::Combo("Quality", &mode, modes, IM_ARRAYSIZE(modes)))
            cfg.upscaler_mode.store(mode);

        float sharp = cfg.sharpness.load();
        if (ImGui::SliderFloat("Sharpness", &sharp, 0.0f, 1.0f)) cfg.sharpness.store(sharp);

        ImGui::Separator();
        bool fg = cfg.framegen_enabled.load();
        if (ImGui::Checkbox("Enable frame generation", &fg)) cfg.framegen_enabled.store(fg);
        ImGui::Text("Real frames:      %llu", (unsigned long long)framegen::real_frames());
        ImGui::Text("Generated frames: %llu", (unsigned long long)framegen::generated_frames());
        ImGui::TextDisabled("DX12 path: overlay + queue capture active; FSR3/FidelityFX wiring is next.");

        ImGui::End();
    }

    bool create_render_targets(IDXGISwapChain* sc) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(sc->GetDesc(&desc))) return false;
        g_format = desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : desc.BufferDesc.Format;
        const UINT buffer_count = desc.BufferCount ? desc.BufferCount : 2;

        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors = buffer_count;
        if (FAILED(g_dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_rtv_heap)))) return false;
        g_rtv_stride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        g_frames.resize(buffer_count);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < buffer_count; ++i) {
            g_frames[i].rtv = cpu;
            if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].backbuffer)))) return false;
            g_dev->CreateRenderTargetView(g_frames[i].backbuffer, nullptr, cpu);
            if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator)))) return false;
            cpu.ptr += g_rtv_stride;
        }

        if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr, IID_PPV_ARGS(&g_cmd))))
            return false;
        g_cmd->Close();
        return true;
    }

    bool init(IDXGISwapChain* sc) {
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&g_sc3))) || !g_sc3) return false;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_dev))) || !g_dev) return false;

        g_queue = hooks::dx12::queue_for_swapchain(sc);
        if (!g_queue) {
            LOGF("[overlay-dx12] no captured ID3D12CommandQueue for swapchain %p", static_cast<void*>(sc));
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;

        // Safety gate: several DX12 games crash if we submit our own command list
        // without a full allocator/fence/backbuffer-state tracking path. Keep DX12
        // queue capture alive by default, but do not render the overlay unless the
        // tester explicitly opts in with FSRINJ_DX12_OVERLAY=1.
        g_render_enabled = env_enabled(L"FSRINJ_DX12_OVERLAY");
        if (!g_render_enabled) {
            LOGF("[overlay-dx12] safe mode: render disabled; set FSRINJ_DX12_OVERLAY=1 to test overlay drawing");
            LOGF("[overlay-dx12] initialized on hwnd %p queue=%p", static_cast<void*>(g_hwnd),
                 static_cast<void*>(g_queue));
            return true;
        }

        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors = 1;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dev->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&g_srv_heap)))) return false;

        if (!create_render_targets(sc)) return false;

        IMGUI_CHECKVERSION();
        if (!ImGui::GetCurrentContext()) ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX12_Init(g_dev, (int)g_frames.size(), g_format, g_srv_heap,
                            g_srv_heap->GetCPUDescriptorHandleForHeapStart(),
                            g_srv_heap->GetGPUDescriptorHandleForHeapStart());

        g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
        LOGF("[overlay-dx12] initialized on hwnd %p buffers=%u queue=%p", static_cast<void*>(g_hwnd),
             (unsigned)g_frames.size(), static_cast<void*>(g_queue));
        return true;
    }
}

bool on_present(IDXGISwapChain* sc) {
    if (!g_init) {
        if (!init(sc)) return false;
        g_init = true;
    }

    static bool prev = false;
    bool down = (GetAsyncKeyState(core::config().toggle_key.load()) & 0x8000) != 0;
    if (down && !prev) {
        bool v = core::config().overlay_visible.load();
        core::config().overlay_visible.store(!v);
    }
    prev = down;

    if (!g_render_enabled) return true;
    if (!core::config().overlay_visible.load()) return true;

    const UINT idx = g_sc3 ? g_sc3->GetCurrentBackBufferIndex() : 0;
    if (idx >= g_frames.size()) return true;
    FrameContext& f = g_frames[idx];

    f.allocator->Reset();
    g_cmd->Reset(f.allocator, nullptr);

    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = f.backbuffer;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cmd->ResourceBarrier(1, &b1);

    g_cmd->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_srv_heap };
    g_cmd->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_menu();
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd);

    D3D12_RESOURCE_BARRIER b2 = b1;
    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmd->ResourceBarrier(1, &b2);
    g_cmd->Close();

    ID3D12CommandList* lists[] = { g_cmd };
    g_queue->ExecuteCommandLists(1, lists);
    return true;
}

void on_resize_buffers() { release_frame_resources(); }
void on_after_resize(IDXGISwapChain* sc) { if (g_init && g_dev) create_render_targets(sc); }

void shutdown() {
    if (g_hwnd && g_orig_wndproc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_wndproc);
    if (g_init && g_render_enabled) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    }
    release_frame_resources();
    if (g_srv_heap) { g_srv_heap->Release(); g_srv_heap = nullptr; }
    if (g_queue) { g_queue->Release(); g_queue = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
    g_render_enabled = false;
    g_init = false;
}

} // namespace overlay::dx12
