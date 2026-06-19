#include "overlay/overlay.h"
#include "overlay/overlay_dx12.h"
#include "core/config.h"
#include "core/log.h"
#include "detect/upscaler_detect.h"
#include "fsr/fsr_integration.h"
#include "fsr/framegen.h"
#include "fsr/upscaler.h"
#include "hooks/depth_hook.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {
namespace {
    bool                    g_init = false;
    bool                    g_dx12_mode = false;
    ID3D11Device*           g_dev  = nullptr;
    ID3D11DeviceContext*    g_ctx  = nullptr;
    ID3D11RenderTargetView* g_rtv  = nullptr;
    HWND                    g_hwnd = nullptr;
    WNDPROC                 g_orig_wndproc = nullptr;
    detect::DetectResult    g_profile;
    bool                    g_profile_done = false;

    LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (core::config().overlay_visible.load() &&
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return true;
        return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
    }

    void create_rtv(IDXGISwapChain* sc) {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
            g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
            back->Release();
        }
    }

    bool init(IDXGISwapChain* sc) {
        HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev);
        if (FAILED(hr)) {
            LOGF("[overlay] GetDevice(ID3D11Device) failed hr=0x%08lX", hr);
            return false;
        }
        LOGF("[overlay] swapchain is DIRECTX 11, proceeding");
        g_dev->GetImmediateContext(&g_ctx);

        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_dev, g_ctx);

        create_rtv(sc);

        g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(
            g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);

        LOGF("[overlay] initialized on hwnd %p", (void*)g_hwnd);
        return true;
    }

    void draw_menu() {
        auto& cfg = core::config();
        ImGui::Begin("FSR Injector");

        if (!g_profile_done) { g_profile = detect::scan_loaded_modules(); g_profile_done = true; }
        ImGui::Text("Game profile: %s", detect::profile_name(g_profile.profile));
        ImGui::Separator();

        bool up = cfg.upscaling_enabled.load();
        if (ImGui::Checkbox("Enable adaptive sharpening", &up)) cfg.upscaling_enabled.store(up);

        const char* modes[] = { "Off","Quality","Balanced","Performance","Ultra Performance" };
        int mode = cfg.upscaler_mode.load();
        if (ImGui::Combo("Quality", &mode, modes, IM_ARRAYSIZE(modes)))
            cfg.upscaler_mode.store(mode);

        float sharp = cfg.sharpness.load();
        if (ImGui::SliderFloat("Sharpness", &sharp, 0.0f, 1.0f)) cfg.sharpness.store(sharp);

        ImGui::Separator();
        ImGui::Text("DX11 Frame Generation");
        bool fg = cfg.framegen_enabled.load();
        if (ImGui::Checkbox("Enable frame generation", &fg)) cfg.framegen_enabled.store(fg);
        bool pacing = cfg.dx11_frame_pacing.load();
        if (ImGui::Checkbox("Generated-frame pacing", &pacing)) cfg.dx11_frame_pacing.store(pacing);
        bool overlayCapture = cfg.dx11_overlay_in_generated.load();
        if (ImGui::Checkbox("Keep menu visible on generated frames", &overlayCapture))
            cfg.dx11_overlay_in_generated.store(overlayCapture);
        if (g_profile.profile == detect::GameProfile::Bare)
            ImGui::TextDisabled("DX11 path: pyramid optical flow + confidence mask");

        ImGui::Text("Real frames:      %llu", (unsigned long long)framegen::real_frames());
        ImGui::Text("Generated frames: %llu", (unsigned long long)framegen::generated_frames());

        ImGui::Separator();
        ImGui::Text("DX11 Depth Assist");
        if (depth::found())
            ImGui::Text("Depth buffer: %ux%u %s %s", depth::width(), depth::height(),
                        depth::fmt_name(), depth::readable() ? "private copy OK" : "private copy pending");
        else
            ImGui::TextDisabled("Depth buffer: not detected yet");
        bool ud = cfg.use_depth.load();
        if (ImGui::Checkbox("Use depth for disocclusion", &ud)) cfg.use_depth.store(ud);
        ImGui::TextDisabled("If the game DSV has no SRV bind, the injector tries a private copy.");

        ImGui::Separator();
        ImGui::TextDisabled("FSR engine: %s", fsr::status_string());

        ImGui::End();
    }
}

void on_present(IDXGISwapChain* sc) {
    static bool first = true;
    if (first) { LOGF("[overlay] on_present fired -- hook reaches our code"); first = false; }

    if (g_dx12_mode) { overlay::dx12::on_present(sc); return; }

    if (!g_init) {
        void* d3d12_probe = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), &d3d12_probe)) && d3d12_probe) {
            reinterpret_cast<IUnknown*>(d3d12_probe)->Release();
            g_dx12_mode = true;
            LOGF("[overlay] swapchain is DIRECTX 12, using D3D12 backend");
            overlay::dx12::on_present(sc);
            return;
        }
        if (!init(sc)) return;
        g_init = true;
    }

    // Toggle on key edge.
    static bool prev = false;
    bool down = (GetAsyncKeyState(core::config().toggle_key.load()) & 0x8000) != 0;
    if (down && !prev) {
        bool v = core::config().overlay_visible.load();
        core::config().overlay_visible.store(!v);
    }
    prev = down;

    if (!core::config().overlay_visible.load() || !g_rtv) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_menu();
    ImGui::Render();

    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void after_present(IDXGISwapChain* sc, unsigned int flags, PresentFn present_fn) {
    if (g_dx12_mode) overlay::dx12::after_present(sc, flags, reinterpret_cast<overlay::dx12::PresentFn>(present_fn));
}

void after_present1(IDXGISwapChain1* sc, unsigned int flags, const DXGI_PRESENT_PARAMETERS* pp, Present1Fn present1_fn) {
    if (g_dx12_mode) overlay::dx12::after_present1(sc, flags, pp, reinterpret_cast<overlay::dx12::Present1Fn>(present1_fn));
}

void on_resize_buffers() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    overlay::dx12::on_resize_buffers();
    framegen::on_resize();
    upscaler::on_resize();
}

void on_after_resize(IDXGISwapChain* sc) {
    if (g_init && g_dev) create_rtv(sc);
    overlay::dx12::on_after_resize(sc);
}

void shutdown() {
    overlay::dx12::shutdown();
    framegen::shutdown();
    upscaler::shutdown();
    if (!g_init) return;
    if (g_hwnd && g_orig_wndproc)
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_wndproc);
    if (g_rtv) g_rtv->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    g_init = false;
    g_dx12_mode = false;
}

} // namespace overlay
