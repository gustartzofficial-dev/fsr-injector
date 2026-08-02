#include "overlay/overlay_dx12.h"
#include "hooks/dx12_queue_capture.h"
#include "core/config.h"
#include "core/log.h"
#include "core/settings.h"
#include "capture/generic_resource_scout.h"
#include "fsr/fsr1_constants.h"

// Embedded copies of AMD's FidelityFX FSR1 headers (MIT), generated at build
// time from third_party/ffx by cmake/embed_text_file.cmake, served to
// D3DCompile through the shared include handler (also used by the DX11 path).
#include "fsr/ffx_include.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <dxgi1_2.h>
#include <cstring>
#include <cwchar>
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
        UINT64 fence_value = 0;
    };

    bool g_init = false;
    IDXGISwapChain3* g_sc3 = nullptr;
    ID3D12Device* g_dev = nullptr;
    ID3D12CommandQueue* g_queue = nullptr;
    ID3D12DescriptorHeap* g_rtv_heap = nullptr;
    ID3D12DescriptorHeap* g_srv_heap = nullptr;
    // --- Dear ImGui overlay (replaces the hand-drawn bitmap UI when available) ---
    bool g_use_imgui = true;                       // env FSRINJ_DX12_IMGUI=0 forces the legacy UI
    bool g_imgui_ready = false;                     // true once ImGui DX12 actually initialized
    ID3D12DescriptorHeap* g_imgui_srv_heap = nullptr; // 1 shader-visible descriptor for the font atlas
    WNDPROC g_orig_wndproc = nullptr;               // for routing input into ImGui
    // ImGui helpers (defined below; forward-declared so init/render can call them)
    bool imgui_init(UINT frame_count);
    void imgui_shutdown();
    bool imgui_begin_frame();
    void imgui_render_to(D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    ID3D12GraphicsCommandList* g_cmd = nullptr;
    ID3D12Fence* g_fence = nullptr;
    ID3D12RootSignature* g_root_sig = nullptr;
    ID3D12PipelineState* g_downscale_pso = nullptr;
    ID3D12PipelineState* g_easu_rcas_pso = nullptr;
    // Real FidelityFX FSR1 path (vendored ffx_fsr1.h): a dedicated EASU pass into
    // g_easu, then genuine RCAS in the final composite pass. When the shaders fail
    // to compile at runtime for any reason, g_fsr_real stays false and the legacy
    // approximation path keeps working exactly as before.
    ID3D12PipelineState* g_fsr_easu_pso = nullptr;
    ID3D12Resource* g_easu = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE g_easu_rtv{};
    D3D12_RESOURCE_STATES g_easu_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bool g_fsr_real = false;
    fsr1::EasuConstants g_easu_con{};
    fsr1::RcasConstants g_rcas_con{};
    float g_rcas_con_for_sharpness = -1.0f;
    ID3D12Resource* g_input = nullptr;
    ID3D12Resource* g_lowres = nullptr;
    ID3D12Resource* g_history = nullptr;
    // Generated interpolated frames. Up to 3 per real frame (4x mode), rendered
    // at t = k/N between the previous and current real frames, DLSS-MFG style.
    static const UINT kMaxGenFrames = 3;
    ID3D12Resource* g_gen[kMaxGenFrames] = {};
    ID3D12Resource* g_scout_motion = nullptr; // private safe copy of scout candidate, never the game-owned resource
    D3D12_RESOURCE_STATES g_scout_motion_state = D3D12_RESOURCE_STATE_COPY_DEST;
    HANDLE g_fence_event = nullptr;
    UINT64 g_next_fence_value = 1;
    std::vector<FrameContext> g_frames;
    UINT g_rtv_stride = 0;
    UINT g_srv_stride = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE g_lowres_rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE g_gen_rtv[kMaxGenFrames] = {};
    DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT g_width = 0;
    UINT g_height = 0;
    HWND g_hwnd = nullptr;
    bool g_effect_allowed = true;
    bool g_effect_enabled = true;
    // Set when a fence wait times out AND the device reports removal (TDR, crash,
    // driver reset). From then on the injector goes fully passive: recording more
    // GPU work on a removed device would take the game down with us.
    bool g_device_lost = false;
    // False on swapchains created with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT.
    // Those games budget exactly one Present per latency-object wait; our extra
    // generated-frame Present consumes a latency slot every frame and stalls them.
    bool g_gen_present_allowed = true;
    bool g_menu_visible = true;
    bool g_logged_first_effect = false;
    bool g_history_ready = false;
    bool g_interpolation_enabled = false;
    bool g_generated_present_enabled = false;
    unsigned g_gen_ready = 0;   // generated frames rendered & pending present this cycle
    unsigned g_gen_mult = 2;    // frame-generation multiplier: 2x, 3x, or 4x
    bool g_scout_motion_enabled = false;        // F6: scout candidate validation enabled
    bool g_scout_motion_bound = false;          // true only when the copied MV texture is actively used by shader
    bool g_scout_motion_copy_ready = false;     // private copy exists and copy commands have been recorded successfully
    bool g_scout_motion_use_enabled = false;    // F7 second press: actually feed copied MV into interpolation/framegen
    bool g_scout_motion_copy_requested = false; // F7 first press: record a copy validation command
    bool g_scout_motion_create_only_logged = false;
    bool g_prev_left_mouse = false;
    bool g_inside_generated_present = false;
    LARGE_INTEGER g_qpc_freq{};
    LARGE_INTEGER g_fps_window_start{};
    // --- frame pacing state ---
    LARGE_INTEGER g_last_real_present_qpc{}; // timestamp of the most recent real present
    double g_real_interval_sec = 0.0;        // smoothed gap between real presents (EMA)
    bool   g_pacing_enabled = true;          // space generated frame toward the temporal midpoint
    double g_pace_fraction = 0.5;            // 0.5 = halfway between two real frames
    HANDLE g_pace_timer = nullptr;           // high-resolution waitable timer for low-CPU pacing
    unsigned g_real_present_samples = 0;
    unsigned g_generated_present_samples = 0;
    unsigned g_generated_present_log_count = 0;
    float g_real_fps = 0.0f;
    float g_output_fps = 0.0f;
    float g_sharpness = 0.20f;
    float g_scale = 0.77f;
    UINT g_low_width = 0;
    UINT g_low_height = 0;
    unsigned g_present_count = 0;
    const unsigned kWarmupPresents = 3;
    D3D12_RESOURCE_STATES g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES g_lowres_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES g_history_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES g_gen_state[kMaxGenFrames] = { D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET };
    DXGI_FORMAT g_scout_motion_format = DXGI_FORMAT_UNKNOWN;
    unsigned g_scout_motion_width = 0;
    unsigned g_scout_motion_height = 0;

    template <class T>
    void safe_release(T*& p) {
        if (p) { p->Release(); p = nullptr; }
    }

    // These now resolve through core::settings: env var first (unchanged
    // behavior), then fsrinj.ini next to the DLL, then the fallback.
    bool env_disabled(const wchar_t* name) {
        return core::settings::has(name) && !core::settings::get_bool(name, true);
    }

    bool opt_bool(const wchar_t* name, bool fallback) {
        return core::settings::get_bool(name, fallback);
    }

    float env_float(const wchar_t* name, float fallback) {
        return core::settings::get_float(name, fallback, 0.0f, 1.0f);
    }

    float env_scale(const wchar_t* name, float fallback) {
        return core::settings::get_float(name, fallback, 0.50f, 1.00f);
    }

    // Hotkey edge detection. GetAsyncKeyState's 0x0001 "pressed since last call"
    // bit is shared system-wide and unreliable; we track edges on the 0x8000 bit
    // ourselves. State always updates (so refocusing doesn't replay a stale edge),
    // but the press only *fires* while the game window is foreground -- otherwise
    // typing in Discord with the game in the background toggles injector features.
    bool key_edge(int vk) {
        static bool prev[256] = {};
        if (vk <= 0 || vk >= 256) return false;
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool fired = down && !prev[vk];
        prev[vk] = down;
        return fired;
    }

    void update_fps_counters(unsigned generated_samples) {
        if (g_qpc_freq.QuadPart == 0) {
            QueryPerformanceFrequency(&g_qpc_freq);
            QueryPerformanceCounter(&g_fps_window_start);
        }
        if (generated_samples) g_generated_present_samples += generated_samples;
        else g_real_present_samples += 1;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - g_fps_window_start.QuadPart) /
                               static_cast<double>(g_qpc_freq.QuadPart ? g_qpc_freq.QuadPart : 1);
        if (elapsed >= 0.50) {
            g_real_fps = static_cast<float>(static_cast<double>(g_real_present_samples) / elapsed);
            g_output_fps = static_cast<float>(static_cast<double>(g_real_present_samples + g_generated_present_samples) / elapsed);
            g_real_present_samples = 0;
            g_generated_present_samples = 0;
            g_fps_window_start = now;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(index) * g_srv_stride;
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(index) * static_cast<UINT64>(g_srv_stride);
        return h;
    }

    bool wait_for_fence(UINT64 value) {
        if (!g_fence || value == 0) return true;
        if (g_fence->GetCompletedValue() >= value) return true;
        HRESULT hr = g_fence->SetEventOnCompletion(value, g_fence_event);
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] SetEventOnCompletion failed hr=0x%08lX", hr);
            return false;
        }
        const DWORD wait = WaitForSingleObject(g_fence_event, 2000);
        if (wait == WAIT_TIMEOUT && g_fence->GetCompletedValue() < value) {
            // A 2s GPU stall usually means TDR/device removal. Previously we fell
            // through and Reset() in-flight allocators, which turns a recoverable
            // hiccup into DXGI_ERROR_DEVICE_REMOVED for the *game*. Instead:
            // diagnose, and if the device is gone, permanently go passive.
            HRESULT reason = g_dev ? g_dev->GetDeviceRemovedReason() : S_OK;
            LOGF("[overlay-dx12] fence wait timed out (value=%llu completed=%llu) removedReason=0x%08lX",
                 (unsigned long long)value, (unsigned long long)g_fence->GetCompletedValue(), reason);
            if (reason != S_OK && !g_device_lost) {
                g_device_lost = true;
                g_effect_enabled = false;
                g_generated_present_enabled = false;
                LOGF("[overlay-dx12] device removed; injector disabled for the rest of the session");
            }
            return false;
        }
        return g_fence->GetCompletedValue() >= value;
    }

    bool wait_for_frame(FrameContext& f) {
        if (!wait_for_fence(f.fence_value)) {
            LOGF("[overlay-dx12] fence wait timed out; skipping sharpen frame");
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

    // Recovery: if Close() or Reset() ever fails, the single shared command list is
    // left stuck in the recording state and every subsequent Reset() returns
    // E_INVALIDARG forever -- which silently kills the overlay until the game exits.
    // Tear the list down and recreate it (closed, ready to Reset) so the next frame
    // can proceed. Returns true if g_cmd is usable again.
    bool recover_command_list() {
        wait_for_gpu_idle();
        safe_release(g_cmd);
        if (g_frames.empty() || !g_frames[0].allocator || !g_dev) return false;
        HRESULT hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              g_frames[0].allocator, nullptr, IID_PPV_ARGS(&g_cmd));
        if (FAILED(hr) || !g_cmd) {
            LOGF("[overlay-dx12] command list recovery failed hr=0x%08lX", hr);
            g_cmd = nullptr;
            return false;
        }
        g_cmd->Close();
        LOGF("[overlay-dx12] command list recovered after failure");
        return true;
    }

    bool compile_shader(const char* source, const char* entry, const char* target, ID3DBlob** blob,
                        const D3D_SHADER_MACRO* defines = nullptr, bool quiet = false) {
        UINT flags = 0;
    #if defined(_DEBUG)
        flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    #endif
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3DCompile(source, std::strlen(source), "fsrinj", defines, &fsr1::include_handler(),
                                entry, target, flags, 0, blob, &errors);
        if (FAILED(hr)) {
            if (errors) {
                LOGF("[overlay-dx12] D3DCompile %s/%s failed hr=0x%08lX%s: %s", entry, target, hr,
                     quiet ? " (falling back)" : "",
                     static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            } else {
                LOGF("[overlay-dx12] D3DCompile %s/%s failed hr=0x%08lX%s", entry, target, hr,
                     quiet ? " (falling back)" : "");
            }
            return false;
        }
        if (errors) errors->Release();
        return true;
    }

    bool create_upscale_pipeline() {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 5;   // t0..t3 legacy inputs + t4 EASU result
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.RegisterSpace = 0;
        params[1].Constants.Num32BitValues = 24;   // 16 legacy params + RCAS con + fsrActive + pad
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MipLODBias = 0.0f;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 2;
        rs_desc.pParameters = params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers = &sampler;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            if (err) {
                LOGF("[overlay-dx12] D3D12SerializeRootSignature failed hr=0x%08lX: %s", hr,
                     static_cast<const char*>(err->GetBufferPointer()));
                err->Release();
            } else {
                LOGF("[overlay-dx12] D3D12SerializeRootSignature failed hr=0x%08lX", hr);
            }
            return false;
        }
        hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root_sig));
        sig->Release();
        if (err) err->Release();
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateRootSignature failed hr=0x%08lX", hr);
            return false;
        }

        const char* hlsl =
R"HLSL(
Texture2D<float4> gInput : register(t0);
Texture2D<float4> gHistory : register(t1);
Texture2D<float4> gLowres : register(t2);
Texture2D<float4> gScoutMotion : register(t3);
Texture2D<float4> gEasu : register(t4);
SamplerState gSampler : register(s0);
cbuffer Params : register(b0) {
    float2 invSize;
    float sharpness;
    float scale;
    float2 invOutputSize;
    float overlayOn;
    float effectOn;
    float historyReady;
    float interpOn;
    float genPresentOn;
    float realFps;
    float outputFps;
    float scoutOn;
    float scoutDepth;
    float scoutMotion;
    uint4 rcasCon;      // FidelityFX RCAS constants (FsrRcasCon)
    float fsrActive;    // 1.0 when the real EASU pass produced gEasu this frame
    float interpT;      // temporal position of this generated frame (0..1, prev->curr)
    float2 fsrPad;
};
#ifdef FSRINJ_REAL_FSR
// Genuine AMD FidelityFX RCAS (MIT), reading the output of the real EASU pass.
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return gEasu.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"
#endif
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 p;
    if (id == 0) p = float2(-1.0, -1.0);
    else if (id == 1) p = float2(-1.0, 3.0);
    else p = float2(3.0, -1.0);
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2(0.5 * p.x + 0.5, -0.5 * p.y + 0.5);
    return o;
}
float luma(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }

float4 DownscalePS(VSOut i) : SV_Target {
    // Test-source creation pass. The game still rendered at full resolution, so this
    // pass intentionally makes an internal low-resolution source that lets us verify
    // the EASU-style upscale + RCAS chain without forcing game resolution yet.
    float2 px = invSize;
    float3 c = gInput.SampleLevel(gSampler, i.uv, 0.0).rgb * 0.40;
    c += gInput.SampleLevel(gSampler, i.uv + float2( px.x, 0.0), 0.0).rgb * 0.15;
    c += gInput.SampleLevel(gSampler, i.uv + float2(-px.x, 0.0), 0.0).rgb * 0.15;
    c += gInput.SampleLevel(gSampler, i.uv + float2(0.0,  px.y), 0.0).rgb * 0.15;
    c += gInput.SampleLevel(gSampler, i.uv + float2(0.0, -px.y), 0.0).rgb * 0.15;
    float a = gInput.SampleLevel(gSampler, i.uv, 0.0).a;
    return float4(c, a);
}

float3 fsr1_rcas(float2 uv, float2 px, float strength) {
    float3 b = gLowres.SampleLevel(gSampler, uv + float2(0.0, -px.y), 0.0).rgb;
    float3 d = gLowres.SampleLevel(gSampler, uv + float2(-px.x, 0.0), 0.0).rgb;
    float3 e = gLowres.SampleLevel(gSampler, uv, 0.0).rgb;
    float3 f = gLowres.SampleLevel(gSampler, uv + float2(px.x, 0.0), 0.0).rgb;
    float3 h = gLowres.SampleLevel(gSampler, uv + float2(0.0, px.y), 0.0).rgb;
    float3 mn = min(e, min(min(b, d), min(f, h)));
    float3 mx = max(e, max(max(b, d), max(f, h)));
    float3 local_range = max(mx - mn, 1.0 / 255.0);
    float center_luma = luma(e);
    float neighbor_luma = 0.25 * (luma(b) + luma(d) + luma(f) + luma(h));
    float detail = center_luma - neighbor_luma;
    float contrast = saturate(local_range.r * 0.299 + local_range.g * 0.587 + local_range.b * 0.114);
    float flat_suppression = saturate(contrast * 8.0);
    float edge_limit = 1.0 - saturate(abs(detail) * 4.0);
    float gain = strength * 0.85 * flat_suppression * (0.35 + 0.65 * edge_limit);
    float3 lap = 4.0 * e - (b + d + f + h);
    float3 outc = e + lap * gain * 0.25;
    float3 margin = local_range * (0.20 + 0.35 * strength);
    return clamp(outc, mn - margin, mx + margin);
}

float3 fsr1_easu(float2 uv) {
    // EASU-inspired reconstruction pass. This is intentionally dependency-free HLSL
    // for the injector path; it is not a verbatim FidelityFX SDK shader yet. It uses
    // directional luma gradients to bias the upscale taps away from high-contrast
    // edges, then applies the existing RCAS-style limiter.
    float2 px = invSize;
    float3 c = gLowres.SampleLevel(gSampler, uv, 0.0).rgb;
    float3 l = gLowres.SampleLevel(gSampler, uv + float2(-px.x, 0.0), 0.0).rgb;
    float3 r = gLowres.SampleLevel(gSampler, uv + float2( px.x, 0.0), 0.0).rgb;
    float3 u = gLowres.SampleLevel(gSampler, uv + float2(0.0, -px.y), 0.0).rgb;
    float3 d = gLowres.SampleLevel(gSampler, uv + float2(0.0,  px.y), 0.0).rgb;
    float3 lu = gLowres.SampleLevel(gSampler, uv + float2(-px.x, -px.y), 0.0).rgb;
    float3 ru = gLowres.SampleLevel(gSampler, uv + float2( px.x, -px.y), 0.0).rgb;
    float3 ld = gLowres.SampleLevel(gSampler, uv + float2(-px.x,  px.y), 0.0).rgb;
    float3 rd = gLowres.SampleLevel(gSampler, uv + float2( px.x,  px.y), 0.0).rgb;

    float gx = abs(luma(l) - luma(r)) + 0.5 * abs(luma(lu + ld) - luma(ru + rd));
    float gy = abs(luma(u) - luma(d)) + 0.5 * abs(luma(lu + ru) - luma(ld + rd));
    float wx = 1.0 / (0.015 + gx);
    float wy = 1.0 / (0.015 + gy);

    float3 axis = (l + r) * wx + (u + d) * wy;
    axis /= max(2.0 * (wx + wy), 0.0001);
    float3 diag = 0.25 * (lu + ru + ld + rd);
    float edge = saturate(abs(gx - gy) * 6.0);
    return lerp(0.55 * c + 0.35 * axis + 0.10 * diag, 0.72 * c + 0.28 * axis, edge);
}

uint glyph_row(uint ch, uint row) {
    // 5x7 uppercase/digit bitmap rows, MSB on the left. This keeps the DX12 UI
    // independent from Dear ImGui while still giving us readable diagnostics.
    if (ch == 32) return 0u;
    if (ch == 46) return row == 6u ? 4u : 0u;
    if (ch == 48) { uint r[7] = {14u,17u,19u,21u,25u,17u,14u}; return r[row]; }
    if (ch == 49) { uint r[7] = {4u,12u,4u,4u,4u,4u,14u}; return r[row]; }
    if (ch == 50) { uint r[7] = {14u,17u,1u,2u,4u,8u,31u}; return r[row]; }
    if (ch == 51) { uint r[7] = {30u,1u,1u,14u,1u,1u,30u}; return r[row]; }
    if (ch == 52) { uint r[7] = {2u,6u,10u,18u,31u,2u,2u}; return r[row]; }
    if (ch == 53) { uint r[7] = {31u,16u,16u,30u,1u,1u,30u}; return r[row]; }
    if (ch == 54) { uint r[7] = {14u,16u,16u,30u,17u,17u,14u}; return r[row]; }
    if (ch == 55) { uint r[7] = {31u,1u,2u,4u,8u,8u,8u}; return r[row]; }
    if (ch == 56) { uint r[7] = {14u,17u,17u,14u,17u,17u,14u}; return r[row]; }
    if (ch == 57) { uint r[7] = {14u,17u,17u,15u,1u,1u,14u}; return r[row]; }
    if (ch == 65) { uint r[7] = {14u,17u,17u,31u,17u,17u,17u}; return r[row]; }
    if (ch == 67) { uint r[7] = {14u,17u,16u,16u,16u,17u,14u}; return r[row]; }
    if (ch == 68) { uint r[7] = {30u,17u,17u,17u,17u,17u,30u}; return r[row]; }
    if (ch == 69) { uint r[7] = {31u,16u,16u,30u,16u,16u,31u}; return r[row]; }
    if (ch == 70) { uint r[7] = {31u,16u,16u,30u,16u,16u,16u}; return r[row]; }
    if (ch == 71) { uint r[7] = {14u,17u,16u,23u,17u,17u,15u}; return r[row]; }
    if (ch == 72) { uint r[7] = {17u,17u,17u,31u,17u,17u,17u}; return r[row]; }
    if (ch == 73) { uint r[7] = {14u,4u,4u,4u,4u,4u,14u}; return r[row]; }
    if (ch == 74) { uint r[7] = {7u,2u,2u,2u,18u,18u,12u}; return r[row]; }
    if (ch == 76) { uint r[7] = {16u,16u,16u,16u,16u,16u,31u}; return r[row]; }
    if (ch == 77) { uint r[7] = {17u,27u,21u,21u,17u,17u,17u}; return r[row]; }
    if (ch == 78) { uint r[7] = {17u,25u,21u,19u,17u,17u,17u}; return r[row]; }
    if (ch == 79) { uint r[7] = {14u,17u,17u,17u,17u,17u,14u}; return r[row]; }
    if (ch == 80) { uint r[7] = {30u,17u,17u,30u,16u,16u,16u}; return r[row]; }
    if (ch == 82) { uint r[7] = {30u,17u,17u,30u,20u,18u,17u}; return r[row]; }
    if (ch == 83) { uint r[7] = {15u,16u,16u,14u,1u,1u,30u}; return r[row]; }
    if (ch == 84) { uint r[7] = {31u,4u,4u,4u,4u,4u,4u}; return r[row]; }
    if (ch == 85) { uint r[7] = {17u,17u,17u,17u,17u,17u,14u}; return r[row]; }
    if (ch == 86) { uint r[7] = {17u,17u,17u,17u,10u,10u,4u}; return r[row]; }
    if (ch == 89) { uint r[7] = {17u,17u,10u,4u,4u,4u,4u}; return r[row]; }
    if (ch == 88) { uint r[7] = {17u,10u,4u,4u,4u,10u,17u}; return r[row]; }
    return 0u;
}

float glyph_pixel(uint ch, float2 pos, float2 origin, float scale_px) {
    float2 q = (pos - origin) / scale_px;
    if (q.x < 0.0 || q.y < 0.0 || q.x >= 5.0 || q.y >= 7.0) return 0.0;
    uint col = (uint)floor(q.x);
    uint row = (uint)floor(q.y);
    uint bits = glyph_row(ch, row);
    return ((bits >> (4u - col)) & 1u) ? 1.0 : 0.0;
}

float text_pixel(uint ch0,uint ch1,uint ch2,uint ch3,uint ch4,uint ch5,uint ch6,uint ch7,uint ch8,uint ch9,uint ch10,uint ch11, float2 pos, float2 origin, float scale_px) {
    uint chars[12] = {ch0,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,ch9,ch10,ch11};
    float hit = 0.0;
    [unroll] for (uint n = 0u; n < 12u; ++n) {
        hit = max(hit, glyph_pixel(chars[n], pos, origin + float2((float)n * 6.0 * scale_px, 0.0), scale_px));
    }
    return hit;
}

float number3_pixel(float value, float2 pos, float2 origin, float scale_px) {
    uint v = (uint)round(clamp(value, 0.0, 999.0));
    uint d0 = (v / 100u) % 10u;
    uint d1 = (v / 10u) % 10u;
    uint d2 = v % 10u;
    float hit = 0.0;
    hit = max(hit, glyph_pixel(48u + d0, pos, origin, scale_px));
    hit = max(hit, glyph_pixel(48u + d1, pos, origin + float2(6.0 * scale_px, 0.0), scale_px));
    hit = max(hit, glyph_pixel(48u + d2, pos, origin + float2(12.0 * scale_px, 0.0), scale_px));
    return hit;
}

float3 draw_native_ui(float3 color, float2 pos) {
    if (overlayOn < 0.5) return color;
    float2 panelMin = float2(14.0, 14.0);
    float2 panelMax = float2(306.0, 286.0);
    float inside = step(panelMin.x, pos.x) * step(panelMin.y, pos.y) * step(pos.x, panelMax.x) * step(pos.y, panelMax.y);
    color = lerp(color, float3(0.025, 0.025, 0.032), inside * 0.72);
    float border = inside * (1.0 - step(panelMin.x + 2.0, pos.x) * step(panelMin.y + 2.0, pos.y) * step(pos.x, panelMax.x - 2.0) * step(pos.y, panelMax.y - 2.0));
    color = lerp(color, float3(0.95, 0.18, 0.95), border);

    float2 o = float2(25.0, 25.0);
    float white = 0.0;
    white = max(white, text_pixel(70,83,82,32,68,88,49,50,32,85,73,32, pos, o, 2.0));       // FSR DX12 UI
    if (effectOn > 0.5) white = max(white, text_pixel(80,79,83,84,32,79,78,32,32,32,32,32, pos, o + float2(0, 24), 2.0)); // POST ON
    else white = max(white, text_pixel(80,79,83,84,32,79,70,70,32,32,32,32, pos, o + float2(0, 24), 2.0)); // POST OFF
    white = max(white, text_pixel(83,67,65,76,69,32,32,32,32,32,32,32, pos, o + float2(0, 44), 2.0)); // SCALE
    white = max(white, text_pixel(83,72,65,82,80,32,32,32,32,32,32,32, pos, o + float2(0, 66), 2.0)); // SHARP
    if (interpOn > 0.5) white = max(white, text_pixel(70,52,32,77,79,84,78,32,79,78,32,32, pos, o + float2(0, 90), 2.0)); // F4 MOTN ON
    else white = max(white, text_pixel(70,52,32,77,79,84,78,32,79,70,70,32, pos, o + float2(0, 90), 2.0)); // F4 MOTN OFF
    if (genPresentOn > 0.5) white = max(white, text_pixel(70,53,32,71,69,78,32,79,78,32,32,32, pos, o + float2(0, 114), 2.0)); // F5 GEN ON
    else white = max(white, text_pixel(70,53,32,71,69,78,32,79,70,70,32,32, pos, o + float2(0, 114), 2.0)); // F5 GEN OFF
    if (scoutOn > 0.5 && scoutMotion > 0.5) white = max(white, text_pixel(70,54,32,77,86,32,32,79,78,32,32,32, pos, o + float2(0, 138), 2.0)); // F6 MV ON
    else white = max(white, text_pixel(70,54,32,77,86,32,32,79,70,70,32,32, pos, o + float2(0, 138), 2.0)); // F6 MV OFF
    if (historyReady > 0.5) white = max(white, text_pixel(72,73,83,84,32,82,69,65,68,89,32,32, pos, o + float2(0, 138), 2.0)); // HIST READY
    else white = max(white, text_pixel(72,73,83,84,32,87,65,82,77,32,32,32, pos, o + float2(0, 138), 2.0)); // HIST WARM
    white = max(white, text_pixel(70,80,83,32,71,65,77,69,32,32,32,32, pos, o + float2(0, 162), 2.0)); // FPS GAME
    white = max(white, number3_pixel(realFps, pos, o + float2(132, 162), 2.0));
    white = max(white, text_pixel(70,80,83,32,79,85,84,32,32,32,32,32, pos, o + float2(0, 186), 2.0)); // FPS OUT
    white = max(white, number3_pixel(outputFps, pos, o + float2(132, 186), 2.0));
    if (scoutOn > 0.5) white = max(white, text_pixel(83,67,79,85,84,32,79,78,32,32,32,32, pos, o + float2(0, 210), 2.0)); // SCOUT ON
    else white = max(white, text_pixel(83,67,79,85,84,32,79,70,70,32,32,32, pos, o + float2(0, 210), 2.0)); // SCOUT OFF
    if (scoutMotion > 0.5) white = max(white, text_pixel(77,86,32,67,65,78,68,32,32,32,32,32, pos, o + float2(0, 234), 2.0)); // MV CAND
    else white = max(white, text_pixel(77,86,32,78,79,78,69,32,32,32,32,32, pos, o + float2(0, 234), 2.0)); // MV NONE
    if (scoutDepth > 0.5) white = max(white, text_pixel(68,69,80,84,72,32,89,69,83,32,32,32, pos, o + float2(0, 258), 2.0)); // DEPTH YES
)HLSL"
R"HLSL(    else white = max(white, text_pixel(68,69,80,84,72,32,78,79,32,32,32,32, pos, o + float2(0, 258), 2.0)); // DEPTH NO
    color = lerp(color, float3(0.92, 0.92, 0.95), white);

    float scaleBg = step(98.0, pos.x) * step(72.0, pos.y) * step(pos.x, 226.0) * step(pos.y, 78.0);
    float scaleFill = step(98.0, pos.x) * step(72.0, pos.y) * step(pos.x, 98.0 + 128.0 * saturate((scale - 0.50) / 0.50)) * step(pos.y, 78.0);
    float sharpBg = step(98.0, pos.x) * step(94.0, pos.y) * step(pos.x, 226.0) * step(pos.y, 100.0);
    float sharpFill = step(98.0, pos.x) * step(94.0, pos.y) * step(pos.x, 98.0 + 128.0 * saturate(sharpness)) * step(pos.y, 100.0);
    color = lerp(color, float3(0.10, 0.10, 0.12), (scaleBg + sharpBg) * 0.80);
    color = lerp(color, float3(0.95, 0.18, 0.95), (scaleFill + sharpFill) * 0.80);
    return color;
}


float patch_error_lvl(float2 prevUv, float2 currUv, float2 spread) {
    // Compares a small cross of taps. 'spread' widens the taps to emulate a coarser
    // pyramid level: large spread = blurry/low-detail comparison (captures big motion),
    // small spread = sharp comparison (locks fine detail).
    float e = 0.0;
    e += abs(luma(gHistory.SampleLevel(gSampler, prevUv, 0.0).rgb) - luma(gInput.SampleLevel(gSampler, currUv, 0.0).rgb)) * 2.0;
    e += abs(luma(gHistory.SampleLevel(gSampler, prevUv + float2( spread.x, 0.0), 0.0).rgb) - luma(gInput.SampleLevel(gSampler, currUv + float2( spread.x, 0.0), 0.0).rgb));
    e += abs(luma(gHistory.SampleLevel(gSampler, prevUv + float2(-spread.x, 0.0), 0.0).rgb) - luma(gInput.SampleLevel(gSampler, currUv + float2(-spread.x, 0.0), 0.0).rgb));
    e += abs(luma(gHistory.SampleLevel(gSampler, prevUv + float2(0.0,  spread.y), 0.0).rgb) - luma(gInput.SampleLevel(gSampler, currUv + float2(0.0,  spread.y), 0.0).rgb));
    e += abs(luma(gHistory.SampleLevel(gSampler, prevUv + float2(0.0, -spread.y), 0.0).rgb) - luma(gInput.SampleLevel(gSampler, currUv + float2(0.0, -spread.y), 0.0).rgb));
    return e;
}

float2 estimate_optical_flow_lite(float2 uv, out float confidence) {
    // Coarse-to-fine ("pyramid") optical flow over the final backbuffers only. We have
    // no real mip chain here, so each level emulates a shrunk copy by widening the
    // compare taps (spread) and the search step. Level 0 reaches far but rough; each
    // later level refines around the previous estimate. This captures large motion
    // (fast pans / low fps) that the old fixed +-6px single search could not see,
    // while keeping every individual search small. Still NOT engine motion vectors.
    float2 px = invOutputSize;
    float2 flow = float2(0.0, 0.0);
    float best = 9999.0;
    float second = 9999.0;

    // Level 0 -- coarse: 5x5 search, wide step (~+-14px reach), blurry compare.
    {
        float2 s = px * 7.0;   // step between candidates
        float2 b = px * 5.0;   // compare spread (coarse detail)
        float2 cBest = flow; best = 9999.0; second = 9999.0;
        [unroll] for (int y = -2; y <= 2; ++y) {
            [unroll] for (int x = -2; x <= 2; ++x) {
                float2 cand = flow + float2((float)x, (float)y) * s;
                float err = patch_error_lvl(uv, uv + cand, b);
                if (err < best) { second = best; best = err; cBest = cand; }
                else if (err < second) second = err;
            }
        }
        flow = cBest;
    }
    // Level 1 -- medium: 3x3 search around the coarse guess.
    {
        float2 s = px * 3.0;
        float2 b = px * 2.0;
        float2 cBest = flow; best = 9999.0; second = 9999.0;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                float2 cand = flow + float2((float)x, (float)y) * s;
                float err = patch_error_lvl(uv, uv + cand, b);
                if (err < best) { second = best; best = err; cBest = cand; }
                else if (err < second) second = err;
            }
        }
        flow = cBest;
    }
    // Level 2 -- fine: 3x3 single-pixel refine, sharp compare.
    {
        float2 s = px * 1.0;
        float2 b = px * 1.0;
        float2 cBest = flow; best = 9999.0; second = 9999.0;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                float2 cand = flow + float2((float)x, (float)y) * s;
                float err = patch_error_lvl(uv, uv + cand, b);
                if (err < best) { second = best; best = err; cBest = cand; }
                else if (err < second) second = err;
            }
        }
        flow = cBest;
    }

    float ambiguity = saturate((second - best) * 4.0);
    float quality = 1.0 - saturate(best * 1.6);
    confidence = saturate(quality * (0.35 + 0.65 * ambiguity));
    return flow;
}

float2 scout_motion_vector(float2 uv, out float mvConfidence) {
    float4 mv = gScoutMotion.SampleLevel(gSampler, uv, 0.0);
    // Candidate buffers are generic. Treat RG as either signed velocity or encoded
    // 0..1 velocity, then clamp hard so false positives do not explode.
    float2 raw = mv.rg;
    float2 signedA = raw;
    float2 signedB = raw * 2.0 - 1.0;
    float2 pick = dot(abs(signedA), float2(1.0, 1.0)) < dot(abs(signedB), float2(1.0, 1.0)) ? signedA : signedB;
    float mag = length(pick);
    mvConfidence = saturate((mag - 0.0003) * 400.0) * (1.0 - saturate(mag * 8.0));
    return clamp(pick * 0.035, float2(-0.08, -0.08), float2(0.08, 0.08));
}

float3 fsr3_lite_interpolate(float2 uv, float3 processedCurr) {
    float conf = 0.0;
    float2 flow = estimate_optical_flow_lite(uv, conf);
    if (scoutOn > 0.5 && scoutMotion > 0.5) {
        float mvConf = 0.0;
        float2 mvFlow = scout_motion_vector(uv, mvConf);
        flow = lerp(flow, mvFlow, mvConf);
        conf = max(conf, mvConf * 0.85);
    }
    // interpT parameterizes WHERE between the two real frames this generated
    // frame sits (0.25 / 0.5 / 0.75 in 4x mode), DLSS-MFG style.
    float t = saturate(interpT);
    float3 prevWarp = gHistory.SampleLevel(gSampler, uv - flow * t, 0.0).rgb;
    float3 currWarp = gInput.SampleLevel(gSampler, uv + flow * (1.0 - t), 0.0).rgb;
    float3 motionFrame = lerp(prevWarp, currWarp, t);
    float3 simpleFrame = lerp(gHistory.SampleLevel(gSampler, uv, 0.0).rgb, processedCurr, saturate(t + 0.05));

    // Reject likely disocclusion/scene-change pixels. This keeps fast cuts and UI
    // from exploding into trails, falling back to the safer preview blend/current.
    float lumDelta = abs(luma(gHistory.SampleLevel(gSampler, uv, 0.0).rgb) - luma(gInput.SampleLevel(gSampler, uv, 0.0).rgb));
    float reject = saturate(lumDelta * 2.5);
    float3 candidate = lerp(simpleFrame, motionFrame, conf);
    float3 result = saturate(lerp(candidate, processedCurr, reject * 0.55));

    // HUD/UI protection: where the two real frames are ~identical (a static overlay),
    // bypass warping so HUD text / crosshairs do not smear as the flow drags them.
    float3 pcol = gHistory.SampleLevel(gSampler, uv, 0.0).rgb;
    float3 ccol = gInput.SampleLevel(gSampler, uv, 0.0).rgb;
    float3 d3 = abs(pcol - ccol);
    float chg = max(d3.r, max(d3.g, d3.b));
    float staticMask = saturate(1.0 - chg * 50.0);
    return lerp(result, ccol, staticMask);
}

float4 EasuRcasPS(VSOut i) : SV_Target {
    float4 base = gInput.SampleLevel(gSampler, i.uv, 0.0);
    float3 color = base.rgb;
    if (effectOn > 0.5) {
#ifdef FSRINJ_REAL_FSR
        if (fsrActive > 0.5) {
            // Real FidelityFX RCAS over the real EASU result rendered into gEasu.
            float3 rc;
            FsrRcasF(rc.r, rc.g, rc.b, AU2(i.pos.xy), rcasCon);
            color = saturate(rc);
        } else {
#endif
        float3 up = fsr1_easu(i.uv);
        float3 sharp = fsr1_rcas(i.uv, invSize, sharpness);
        float amount = saturate(0.45 + sharpness * 0.55);
        color = saturate(lerp(up, sharp, amount));
#ifdef FSRINJ_REAL_FSR
        }
#endif
    }
    if (interpOn > 0.5 && historyReady > 0.5) {
        // Motion-aware FSR3-lite preview. It estimates optical flow from the final
        // backbuffers only, then blends a warped previous/current frame. This is
        // still not native FSR3 because we do not have engine motion vectors/depth.
        color = fsr3_lite_interpolate(i.uv, color);
    }
    color = draw_native_ui(color, i.pos.xy);
    return float4(color, base.a);
})HLSL";

        // The standalone EASU pass: genuine AMD FidelityFX EASU (MIT) rendering the
        // low-res source up to full resolution into g_easu. RCAS then runs in the
        // main composite shader above.
        const char* fsr_easu_hlsl =
R"HLSL(
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"
cbuffer EasuCB : register(b0) { uint4 con0; uint4 con1; uint4 con2; uint4 con3; };
Texture2D<float4> gLowres : register(t2);
SamplerState gSampler : register(s0);
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return gLowres.GatherRed(gSampler, p); }
AF4 FsrEasuGF(AF2 p) { return gLowres.GatherGreen(gSampler, p); }
AF4 FsrEasuBF(AF2 p) { return gLowres.GatherBlue(gSampler, p); }
#include "ffx_fsr1.h"
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 p;
    if (id == 0) p = float2(-1.0, -1.0);
    else if (id == 1) p = float2(-1.0, 3.0);
    else p = float2(3.0, -1.0);
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2(0.5 * p.x + 0.5, -0.5 * p.y + 0.5);
    return o;
}
float4 EasuPS(VSOut i) : SV_Target {
    AF3 c = AF3(0.0, 0.0, 0.0);
    FsrEasuF(c, AU2(i.pos.xy), con0, con1, con2, con3);
    return float4(c, 1.0);
}
)HLSL";

        ID3DBlob* vs = nullptr;
        ID3DBlob* ps_down = nullptr;
        ID3DBlob* ps_easu = nullptr;
        ID3DBlob* vs_fsr = nullptr;
        ID3DBlob* ps_fsr_easu = nullptr;

        // Try the real-FidelityFX build of the composite shader first; if the
        // vendored headers ever fail to compile on some runtime, fall back to the
        // legacy dependency-free build so the injector keeps working.
        const D3D_SHADER_MACRO fsr_defines[] = { { "FSRINJ_REAL_FSR", "1" }, { nullptr, nullptr } };
        g_fsr_real = compile_shader(hlsl, "VSMain", "vs_5_0", &vs, fsr_defines, true) &&
                     compile_shader(hlsl, "DownscalePS", "ps_5_0", &ps_down, fsr_defines, true) &&
                     compile_shader(hlsl, "EasuRcasPS", "ps_5_0", &ps_easu, fsr_defines, true) &&
                     compile_shader(fsr_easu_hlsl, "VSMain", "vs_5_0", &vs_fsr, nullptr, true) &&
                     compile_shader(fsr_easu_hlsl, "EasuPS", "ps_5_0", &ps_fsr_easu, nullptr, true);
        if (!g_fsr_real) {
            safe_release(vs); safe_release(ps_down); safe_release(ps_easu);
            safe_release(vs_fsr); safe_release(ps_fsr_easu);
            LOGF("[overlay-dx12] FidelityFX FSR1 shader path unavailable; using legacy approximation shaders");
            if (!compile_shader(hlsl, "VSMain", "vs_5_0", &vs)) return false;
            if (!compile_shader(hlsl, "DownscalePS", "ps_5_0", &ps_down)) { vs->Release(); return false; }
            if (!compile_shader(hlsl, "EasuRcasPS", "ps_5_0", &ps_easu)) { vs->Release(); ps_down->Release(); return false; }
        }

        auto fill_pso = [&](ID3DBlob* ps, ID3D12PipelineState** out) -> bool {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_root_sig;
            pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            pso.BlendState.AlphaToCoverageEnable = FALSE;
            pso.BlendState.IndependentBlendEnable = FALSE;
            const D3D12_RENDER_TARGET_BLEND_DESC rt_blend = {
                FALSE, FALSE,
                D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                D3D12_LOGIC_OP_NOOP,
                D3D12_COLOR_WRITE_ENABLE_ALL
            };
            for (auto& rt : pso.BlendState.RenderTarget) rt = rt_blend;
            pso.SampleMask = UINT_MAX;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.RasterizerState.FrontCounterClockwise = FALSE;
            pso.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
            pso.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
            pso.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
            pso.RasterizerState.DepthClipEnable = TRUE;
            pso.RasterizerState.MultisampleEnable = FALSE;
            pso.RasterizerState.AntialiasedLineEnable = FALSE;
            pso.RasterizerState.ForcedSampleCount = 0;
            pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            pso.DepthStencilState.DepthEnable = FALSE;
            pso.DepthStencilState.StencilEnable = FALSE;
            pso.InputLayout = { nullptr, 0 };
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = g_format;
            pso.SampleDesc.Count = 1;
            pso.SampleDesc.Quality = 0;
            HRESULT pso_hr = g_dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(out));
            if (FAILED(pso_hr)) {
                LOGF("[overlay-dx12] CreateGraphicsPipelineState failed hr=0x%08lX format=%u", pso_hr, (unsigned)g_format);
                return false;
            }
            return true;
        };

        bool ok = fill_pso(ps_down, &g_downscale_pso) && fill_pso(ps_easu, &g_easu_rcas_pso);
        if (ok && g_fsr_real) {
            // Same fixed-function state as the other passes, but with the FSR
            // wrapper's own VS/PS blobs. Failure here just drops back to legacy.
            ID3DBlob* saved_vs = vs;
            vs = vs_fsr;
            if (!fill_pso(ps_fsr_easu, &g_fsr_easu_pso)) {
                g_fsr_real = false;
                safe_release(g_fsr_easu_pso);
                LOGF("[overlay-dx12] FSR EASU PSO creation failed; using legacy approximation shaders");
            }
            vs = saved_vs;
        }
        if (ok) LOGF("[overlay-dx12] upscale pipeline ready (%s)",
                     g_fsr_real ? "AMD FidelityFX FSR 1.0 EASU+RCAS" : "legacy approximation");
        vs->Release();
        ps_down->Release();
        ps_easu->Release();
        safe_release(vs_fsr);
        safe_release(ps_fsr_easu);
        return ok;
    }

    bool create_upscale_resources_from_backbuffer(ID3D12Resource* backbuffer) {
        if (!backbuffer) return false;
        D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
        g_width = static_cast<UINT>(desc.Width);
        g_height = desc.Height;
        g_format = desc.Format == DXGI_FORMAT_UNKNOWN ? g_format : desc.Format;

        g_low_width = static_cast<UINT>(static_cast<float>(g_width) * g_scale + 0.5f);
        g_low_height = static_cast<UINT>(static_cast<float>(g_height) * g_scale + 0.5f);
        if (g_low_width < 16) g_low_width = 16;
        if (g_low_height < 16) g_low_height = 16;
        if (g_low_width > g_width) g_low_width = g_width;
        if (g_low_height > g_height) g_low_height = g_height;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC input_desc = desc;
        input_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        input_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        input_desc.SampleDesc.Count = 1;
        input_desc.SampleDesc.Quality = 0;

        HRESULT hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &input_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&g_input));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateCommittedResource(input texture) failed hr=0x%08lX %ux%u fmt=%u", hr, g_width, g_height, (unsigned)g_format);
            return false;
        }
        g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;

        D3D12_RESOURCE_DESC low_desc = input_desc;
        low_desc.Width = g_low_width;
        low_desc.Height = g_low_height;
        low_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = g_format;
        clear.Color[0] = 0.0f;
        clear.Color[1] = 0.0f;
        clear.Color[2] = 0.0f;
        clear.Color[3] = 1.0f;
        hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &low_desc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                            IID_PPV_ARGS(&g_lowres));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateCommittedResource(lowres texture) failed hr=0x%08lX %ux%u fmt=%u", hr, g_low_width, g_low_height, (unsigned)g_format);
            return false;
        }
        g_lowres_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

        hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &input_desc,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                            IID_PPV_ARGS(&g_history));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateCommittedResource(history texture) failed hr=0x%08lX %ux%u fmt=%u", hr, g_width, g_height, (unsigned)g_format);
            return false;
        }
        g_history_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_RESOURCE_DESC gen_desc = input_desc;
        gen_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE gen_clear{};
        gen_clear.Format = g_format;
        gen_clear.Color[3] = 1.0f;
        for (UINT k = 0; k < kMaxGenFrames; ++k) {
            hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &gen_desc,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &gen_clear,
                                                IID_PPV_ARGS(&g_gen[k]));
            if (FAILED(hr)) {
                LOGF("[overlay-dx12] CreateCommittedResource(generated texture %u) failed hr=0x%08lX %ux%u fmt=%u", k, hr, g_width, g_height, (unsigned)g_format);
                return false;
            }
            g_gen_state[k] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        g_history_ready = false;
        g_gen_ready = 0;

        // Full-resolution target for the real FidelityFX EASU pass. Created
        // unconditionally because the shader pipeline (which decides g_fsr_real)
        // is compiled *after* resources; the render path only uses FSR when the
        // texture, the PSO, and the shader build are all present.
        {
            D3D12_RESOURCE_DESC easu_desc = input_desc;
            easu_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE easu_clear{};
            easu_clear.Format = g_format;
            easu_clear.Color[3] = 1.0f;
            hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &easu_desc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &easu_clear,
                                                IID_PPV_ARGS(&g_easu));
            if (FAILED(hr)) {
                LOGF("[overlay-dx12] CreateCommittedResource(easu texture) failed hr=0x%08lX; real FSR path unavailable", hr);
                g_easu = nullptr;
            }
            g_easu_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // Precompute the FidelityFX constants for this resolution pair.
        fsr1::easu_constants(g_easu_con,
                             (float)g_low_width, (float)g_low_height,
                             (float)g_width, (float)g_height);
        g_rcas_con_for_sharpness = -1.0f; // force refresh on next frame

        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors = 5;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dev->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&g_srv_heap));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateDescriptorHeap(SRV) failed hr=0x%08lX", hr);
            return false;
        }
        g_srv_stride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = g_format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        view.Texture2D.MostDetailedMip = 0;
        view.Texture2D.PlaneSlice = 0;
        view.Texture2D.ResourceMinLODClamp = 0.0f;
        g_dev->CreateShaderResourceView(g_input, &view, srv_cpu(0));
        g_dev->CreateShaderResourceView(g_history, &view, srv_cpu(1));
        g_dev->CreateShaderResourceView(g_lowres, &view, srv_cpu(2));
        g_dev->CreateShaderResourceView(nullptr, &view, srv_cpu(3));
        g_dev->CreateShaderResourceView(g_easu, &view, srv_cpu(4));
        return true;
    }

    void release_frame_resources() {
        wait_for_gpu_idle();
        for (auto& f : g_frames) {
            safe_release(f.backbuffer);
            safe_release(f.allocator);
            f.fence_value = 0;
        }
        g_frames.clear();
        safe_release(g_cmd);
        safe_release(g_input);
        safe_release(g_lowres);
        safe_release(g_history);
        for (UINT k = 0; k < kMaxGenFrames; ++k) { safe_release(g_gen[k]); g_gen_state[k] = D3D12_RESOURCE_STATE_RENDER_TARGET; }
        safe_release(g_easu);
        g_easu_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        safe_release(g_scout_motion);
        g_scout_motion_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_scout_motion_bound = false;
        g_scout_motion_copy_ready = false;
        g_scout_motion_create_only_logged = false;
        g_scout_motion_copy_requested = false;
        safe_release(g_srv_heap);
        safe_release(g_downscale_pso);
        safe_release(g_easu_rcas_pso);
        safe_release(g_fsr_easu_pso);
        safe_release(g_root_sig);
        safe_release(g_rtv_heap);
        g_width = 0;
        g_height = 0;
        g_low_width = 0;
        g_low_height = 0;
        g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_lowres_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_history_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_history_ready = false;
        g_gen_ready = 0;
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
        rtv_desc.NumDescriptors = buffer_count + 2 + kMaxGenFrames;   // backbuffers + lowres + easu + generated[3]
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
            if (i == 0) {
                D3D12_RESOURCE_DESC bb = g_frames[i].backbuffer->GetDesc();
                g_width = static_cast<UINT>(bb.Width);
                g_height = bb.Height;
                g_format = bb.Format == DXGI_FORMAT_UNKNOWN ? g_format : bb.Format;
            }
            g_dev->CreateRenderTargetView(g_frames[i].backbuffer, nullptr, cpu);
            hr = g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator));
            if (FAILED(hr)) {
                LOGF("[overlay-dx12] CreateCommandAllocator(%u) failed hr=0x%08lX", i, hr);
                return false;
            }
            cpu.ptr += g_rtv_stride;
        }

        g_lowres_rtv = cpu;
        cpu.ptr += g_rtv_stride;
        g_easu_rtv = cpu;
        for (UINT k = 0; k < kMaxGenFrames; ++k) { cpu.ptr += g_rtv_stride; g_gen_rtv[k] = cpu; }
        if (!create_upscale_resources_from_backbuffer(g_frames[0].backbuffer)) return false;
        g_dev->CreateRenderTargetView(g_lowres, nullptr, g_lowres_rtv);
        if (g_easu) g_dev->CreateRenderTargetView(g_easu, nullptr, g_easu_rtv);
        for (UINT k = 0; k < kMaxGenFrames; ++k)
            if (g_gen[k]) g_dev->CreateRenderTargetView(g_gen[k], nullptr, g_gen_rtv[k]);
        if (!create_upscale_pipeline()) return false;

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

        // Waitable swapchains budget exactly one Present per frame-latency wait.
        // Presenting an extra generated frame consumes their latency slots and
        // stalls the game, so generated-present is disabled on those chains.
        g_gen_present_allowed = (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == 0;
        const bool force_gen = core::settings::has(L"FSRINJ_GENPRESENT_FORCE")
                             ? opt_bool(L"FSRINJ_GENPRESENT_FORCE", false)
                             : opt_bool(L"FSRINJ_DX12_GENPRESENT_FORCE", false);
        if (!g_gen_present_allowed && force_gen) {
            g_gen_present_allowed = true;
            LOGF("[overlay-dx12] WARNING: waitable swapchain detected but FSRINJ_DX12_GENPRESENT_FORCE=1; generated-frame presentation enabled anyway (expect possible stalls/judder)");
        } else if (!g_gen_present_allowed) {
            LOGF("[overlay-dx12] swapchain uses a frame-latency waitable object; generated-frame presentation disabled for compatibility (set FSRINJ_DX12_GENPRESENT_FORCE=1 to test anyway)");
        }

        g_effect_allowed = !env_disabled(L"FSRINJ_DX12_SHARPEN");
        g_effect_enabled = g_effect_allowed;
        g_menu_visible = core::config().overlay_visible.load();
        g_sharpness = env_float(L"FSRINJ_DX12_SHARPNESS", core::config().sharpness.load());
        if (g_sharpness <= 0.0f) g_sharpness = 0.20f;
        if (g_sharpness > 1.0f) g_sharpness = 1.0f;
        g_scale = env_scale(L"FSRINJ_DX12_SCALE", 0.77f);
        g_interpolation_enabled = opt_bool(L"FSRINJ_DX12_INTERP", false);
        g_generated_present_enabled = g_gen_present_allowed && opt_bool(L"FSRINJ_DX12_GENPRESENT", false);
        g_gen_mult = (unsigned)(core::settings::has(L"FSRINJ_GENMULT")
                                ? core::settings::get_int(L"FSRINJ_GENMULT", 2, 2, 4)
                                : core::settings::get_int(L"FSRINJ_DX12_GENMULT", 2, 2, 4));
        g_scout_motion_enabled = opt_bool(L"FSRINJ_DX12_SCOUT_MV", false);
        g_scout_motion_use_enabled = opt_bool(L"FSRINJ_DX12_SCOUT_MV_USE", false);
        g_pacing_enabled = !env_disabled(L"FSRINJ_DX12_PACING");
        g_use_imgui = !env_disabled(L"FSRINJ_DX12_IMGUI");
        g_pace_fraction = (double)env_float(L"FSRINJ_DX12_PACE_FRACTION", 0.5f);
        if (g_pace_fraction < 0.1) g_pace_fraction = 0.1;
        if (g_pace_fraction > 0.9) g_pace_fraction = 0.9;
        g_last_real_present_qpc = LARGE_INTEGER{};
        g_real_interval_sec = 0.0;
        if (!g_pace_timer) {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
            g_pace_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!g_pace_timer) g_pace_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }

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

        if (g_use_imgui && !imgui_init(static_cast<UINT>(g_frames.size()))) {
            LOGF("[overlay-dx12] ImGui init failed; falling back to legacy hand-drawn UI");
            imgui_shutdown();
            g_use_imgui = false;
        } else if (g_use_imgui) {
            LOGF("[overlay-dx12] ImGui DX12 overlay active (mouse + software cursor)");
        }

        capture::scout::note_dx12_swapchain(g_width, g_height, g_format);
        capture::scout::note_final_frame_motion(true);
        capture::scout::log_snapshot_once();

        LOGF("[overlay-dx12] %s + native UI initialized on hwnd %p buffers=%u size=%ux%u lowres=%ux%u scale=%.2f format=%u queue=%p enabled=%s sharpness=%.2f genpresent=%s genmult=%ux",
             g_fsr_real ? "AMD FidelityFX FSR 1.0 (EASU+RCAS)" : "FSR1-style EASU/RCAS (legacy approximation)",
             static_cast<void*>(g_hwnd), (unsigned)g_frames.size(), g_width, g_height, g_low_width, g_low_height, g_scale, (unsigned)g_format,
             static_cast<void*>(g_queue), g_effect_enabled ? "on" : "off", g_sharpness, g_generated_present_enabled ? "on" : "off", g_gen_mult);
        LOGF("[overlay-dx12] Native DX12 settings overlay is enabled; Home=menu End=effect PgUp/PgDn=sharpness Insert/Delete=scale F1/F2/F3=presets F4=motion-preview F5=generated-present F6=scout-MV validate F7=scout-MV use F8=FG multiplier (2x/3x/4x); mouse clicks supported");
        return true;
    }

    void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (!res || before == after) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    }

    void bind_fullscreen_state(D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT width, UINT height,
                               ID3D12PipelineState* pso,
                               float inv_x, float inv_y, float sharpness, float scale,
                               bool menu_visible, bool effect_enabled, bool history_ready, bool interp_enabled, bool gen_present_enabled,
                               float real_fps, float output_fps, float scout_on_arg, float /*scout_depth_arg*/, float scout_motion_arg,
                               bool fsr_active = false, float interp_t = 0.5f) {
        D3D12_VIEWPORT vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(width);
        vp.Height = static_cast<float>(height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g_cmd->RSSetViewports(1, &vp);
        g_cmd->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[] = { g_srv_heap };
        g_cmd->SetDescriptorHeaps(1, heaps);
        g_cmd->SetGraphicsRootSignature(g_root_sig);
        g_cmd->SetPipelineState(pso);
        g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_cmd->SetGraphicsRootDescriptorTable(0, srv_gpu(0));
        auto scout = capture::scout::snapshot();
        const bool scout_depth_ready = scout.dx11_depth_found || scout.dx12_depth_candidates > 0;
        const bool scout_motion_ready = scout.dx12_motion_candidates > 0 || scout.final_frame_motion;
        const float scout_use = scout_on_arg > 0.5f ? 1.0f : 0.0f;
        const float scout_mv_ready = scout_motion_arg > 0.5f ? 1.0f : (scout_motion_ready ? 1.0f : 0.0f);
        const float params[16] = { inv_x, inv_y, sharpness, scale, 1.0f / static_cast<float>(g_width ? g_width : width), 1.0f / static_cast<float>(g_height ? g_height : height), menu_visible ? 1.0f : 0.0f, effect_enabled ? 1.0f : 0.0f, history_ready ? 1.0f : 0.0f, interp_enabled ? 1.0f : 0.0f, gen_present_enabled ? 1.0f : 0.0f, real_fps, output_fps, scout_use, scout_depth_ready ? 1.0f : 0.0f, scout_mv_ready };
        // Root constants are typeless 32-bit values: floats first, then the RCAS
        // uint constants, then the fsrActive flag (matches the cbuffer layout).
        UINT32 blob[24];
        std::memcpy(blob, params, sizeof(params));
        std::memcpy(blob + 16, g_rcas_con.con, sizeof(g_rcas_con.con));
        const float fsr_active_f = fsr_active ? 1.0f : 0.0f;
        std::memcpy(blob + 20, &fsr_active_f, sizeof(float));
        std::memcpy(blob + 21, &interp_t, sizeof(float));
        blob[22] = blob[23] = 0;
        g_cmd->SetGraphicsRoot32BitConstants(1, 24, blob, 0);
        g_cmd->DrawInstanced(3, 1, 0, 0);
    }

    // Real FidelityFX EASU pass: reads the low-res texture (t2), writes the
    // full-resolution reconstruction into g_easu. Constants are AMD's packed
    // uint4 blocks computed on the CPU by the very same vendored header.
    void bind_easu_pass() {
        D3D12_VIEWPORT vp{};
        vp.Width = static_cast<float>(g_width);
        vp.Height = static_cast<float>(g_height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        D3D12_RECT scissor{ 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };
        g_cmd->OMSetRenderTargets(1, &g_easu_rtv, FALSE, nullptr);
        g_cmd->RSSetViewports(1, &vp);
        g_cmd->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[] = { g_srv_heap };
        g_cmd->SetDescriptorHeaps(1, heaps);
        g_cmd->SetGraphicsRootSignature(g_root_sig);
        g_cmd->SetPipelineState(g_fsr_easu_pso);
        g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_cmd->SetGraphicsRootDescriptorTable(0, srv_gpu(0));
        g_cmd->SetGraphicsRoot32BitConstants(1, 16, g_easu_con.con, 0);
        g_cmd->DrawInstanced(3, 1, 0, 0);
    }


    bool create_scout_motion_copy(ID3D12Resource* candidate, DXGI_FORMAT fmt, unsigned w, unsigned h) {
        if (!candidate || !g_dev || !g_srv_heap || w == 0 || h == 0) return false;
        if (g_scout_motion && g_scout_motion_width == w && g_scout_motion_height == h && g_scout_motion_format == fmt) return true;

        safe_release(g_scout_motion);
        g_scout_motion_width = 0;
        g_scout_motion_height = 0;
        g_scout_motion_format = DXGI_FORMAT_UNKNOWN;
        g_scout_motion_state = D3D12_RESOURCE_STATE_COPY_DEST;

        // Do not clone the game's resource desc blindly. Some engines create velocity-like
        // buffers with flags/layout/mip/array properties that are invalid for our private
        // committed copy. Create a clean SRV-compatible 2D texture and copy only subresource 0.
        D3D12_RESOURCE_DESC src_desc = candidate->GetDesc();
        D3D12_RESOURCE_DESC copy_desc{};
        copy_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        copy_desc.Alignment = 0;
        copy_desc.Width = w ? w : static_cast<UINT>(src_desc.Width);
        copy_desc.Height = h ? h : src_desc.Height;
        copy_desc.DepthOrArraySize = 1;
        copy_desc.MipLevels = 1;
        copy_desc.Format = fmt == DXGI_FORMAT_UNKNOWN ? src_desc.Format : fmt;
        copy_desc.SampleDesc.Count = 1;
        copy_desc.SampleDesc.Quality = 0;
        copy_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        copy_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        HRESULT hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&g_scout_motion));
        if (FAILED(hr) || !g_scout_motion) {
            static HRESULT last_hr = S_OK;
            static unsigned fail_count = 0;
            fail_count++;
            if (hr != last_hr || fail_count <= 3 || (fail_count % 120) == 0) {
                LOGF("[overlay-dx12] scout MV safe-copy texture create failed hr=0x%08lX %ux%u fmt=%u srcDim=%u srcMip=%u srcArray=%u srcFlags=0x%X",
                     hr, w, h, (unsigned)fmt, (unsigned)src_desc.Dimension, (unsigned)src_desc.MipLevels, (unsigned)src_desc.DepthOrArraySize, (unsigned)src_desc.Flags);
                last_hr = hr;
            }
            return false;
        }
        g_scout_motion_state = D3D12_RESOURCE_STATE_COPY_DEST;

        D3D12_SHADER_RESOURCE_VIEW_DESC mv_view{};
        mv_view.Format = fmt;
        mv_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        mv_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        mv_view.Texture2D.MipLevels = 1;
        mv_view.Texture2D.MostDetailedMip = 0;
        mv_view.Texture2D.PlaneSlice = 0;
        mv_view.Texture2D.ResourceMinLODClamp = 0.0f;
        g_dev->CreateShaderResourceView(g_scout_motion, &mv_view, srv_cpu(3));
        g_scout_motion_width = w;
        g_scout_motion_height = h;
        g_scout_motion_format = fmt;
        LOGF("[overlay-dx12] scout MV safe-copy texture created %ux%u fmt=%u", w, h, (unsigned)fmt);
        return true;
    }

    void update_scout_motion_binding() {
        g_scout_motion_bound = false;
        if (!g_scout_motion_enabled || !g_dev || !g_srv_heap || !g_cmd) return;

        ID3D12Resource* candidate = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        unsigned w = 0, h = 0;
        D3D12_RESOURCE_STATES candidate_state = D3D12_RESOURCE_STATE_COMMON;
        bool state_known = false;
        if (!capture::scout::acquire_dx12_best_motion_candidate(&candidate, &fmt, &w, &h, &candidate_state, &state_known) || !candidate) return;

        if (!state_known) {
            static bool logged_unknown = false;
            if (!logged_unknown) {
                LOGF("[overlay-dx12] scout MV candidate found %ux%u fmt=%u but skipped: resource state unknown", w, h, (unsigned)fmt);
                logged_unknown = true;
            }
            candidate->Release();
            return;
        }

        const bool already_had_copy = (g_scout_motion && g_scout_motion_width == w && g_scout_motion_height == h && g_scout_motion_format == fmt);
        if (!create_scout_motion_copy(candidate, fmt, w, h)) {
            candidate->Release();
            return;
        }

        // Stage 1: F6 only validates candidate discovery and private texture creation.
        // Do not copy from, transition, or sample the game-owned resource yet. The previous
        // build crashed immediately after creation, so this separates creation from copy/use.
        if (!g_scout_motion_copy_requested && !g_scout_motion_copy_ready) {
            if (!already_had_copy && !g_scout_motion_create_only_logged) {
                LOGF("[overlay-dx12] scout MV stage1 create-only OK; press F7 to record copy validation");
                g_scout_motion_create_only_logged = true;
            }
            candidate->Release();
            return;
        }

        // Stage 2: copy validation only. Still do not bind the copied texture to the shader
        // unless F7 is pressed again after copy_ready becomes true.
        if (g_scout_motion_copy_requested && !g_scout_motion_copy_ready) {
            // Guard: CopyTextureRegion cannot copy from a multisampled or non-2D
            // resource. The flooded "Close failed" brick came from trying to copy an
            // MSAA RESOLVE_SOURCE buffer the heuristic wrongly tagged as motion data.
            D3D12_RESOURCE_DESC cand_desc = candidate->GetDesc();
            if (cand_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || cand_desc.SampleDesc.Count > 1) {
                LOGF("[overlay-dx12] scout MV stage2 SKIPPED: candidate not plain-copyable (dim=%u samples=%u). "
                     "This candidate is almost certainly not a velocity buffer.",
                     (unsigned)cand_desc.Dimension, (unsigned)cand_desc.SampleDesc.Count);
                g_scout_motion_copy_requested = false;
                candidate->Release();
                return;
            }
            LOGF("[overlay-dx12] scout MV stage2 copy validation begin %ux%u fmt=%u srcState=0x%X",
                 w, h, (unsigned)fmt, (unsigned)candidate_state);
            transition(g_cmd, candidate, candidate_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (g_scout_motion_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                transition(g_cmd, g_scout_motion, g_scout_motion_state, D3D12_RESOURCE_STATE_COPY_DEST);
                g_scout_motion_state = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_scout_motion;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = candidate;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            transition(g_cmd, g_scout_motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_scout_motion_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            transition(g_cmd, candidate, D3D12_RESOURCE_STATE_COPY_SOURCE, candidate_state);
            g_scout_motion_copy_ready = true;
            g_scout_motion_copy_requested = false;
            LOGF("[overlay-dx12] scout MV stage2 copy validation commands recorded; press F7 again to use copied MV");
            candidate->Release();
            return;
        }

        candidate->Release();
        if (g_scout_motion_use_enabled && g_scout_motion_copy_ready) {
            g_scout_motion_bound = true;
        }
    }

    LRESULT CALLBACK imgui_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        if (g_imgui_ready && g_menu_visible) {
            if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
        }
        return CallWindowProcW(g_orig_wndproc, h, msg, w, l);
    }

    bool imgui_init(UINT frame_count) {
        if (g_imgui_ready) return true;
        if (!g_dev || !g_hwnd) return false;
        if (frame_count < 2) frame_count = 2;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;   // classic backend only needs one descriptor (the font atlas)
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_imgui_srv_heap))) || !g_imgui_srv_heap)
            return false;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // don't drop an imgui.ini next to the game exe
        ImGui::StyleColorsDark();
        bool w32 = ImGui_ImplWin32_Init(g_hwnd);
        bool dx = w32 && ImGui_ImplDX12_Init(g_dev, (int)frame_count, g_format, g_imgui_srv_heap,
                                             g_imgui_srv_heap->GetCPUDescriptorHandleForHeapStart(),
                                             g_imgui_srv_heap->GetGPUDescriptorHandleForHeapStart());
        if (!w32 || !dx) {
            if (dx) ImGui_ImplDX12_Shutdown();
            if (w32) ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            safe_release(g_imgui_srv_heap);
            return false;
        }
        g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)imgui_wndproc);

        // Build ImGui's device objects now, at init, instead of letting the backend do
        // it lazily on the first ImGui_ImplDX12_NewFrame(). On v1.90.9 that lazy path
        // creates a temporary command queue + list and blocks on a fence to upload the
        // font atlas -- doing that mid-Present, on the render thread, was the most likely
        // source of the DX12 start crash. The scout guard keeps our generic command-list
        // hooks from detouring ImGui's internal upload list while it runs.
        capture::scout::set_overlay_active(true);
        const bool objs_ok = ImGui_ImplDX12_CreateDeviceObjects();
        capture::scout::set_overlay_active(false);
        if (!objs_ok) {
            LOGF("[overlay-dx12] ImGui_ImplDX12_CreateDeviceObjects failed; disabling ImGui overlay");
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_wndproc);
            g_orig_wndproc = nullptr;
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            safe_release(g_imgui_srv_heap);
            return false;
        }

        g_imgui_ready = true;
        return true;
    }

    void imgui_shutdown() {
        if (g_hwnd && g_orig_wndproc) {
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_wndproc);
            g_orig_wndproc = nullptr;
        }
        if (g_imgui_ready) {
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_imgui_ready = false;
        }
        safe_release(g_imgui_srv_heap);
    }

    void imgui_build_menu() {
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("FSR Injector  (DirectX 12)", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("FPS   game %.0f    output %.0f", g_real_fps, g_output_fps);
        ImGui::Separator();
        ImGui::Checkbox("Post-process (sharpen / upscale)", &g_effect_enabled);
        ImGui::SliderFloat("Sharpness", &g_sharpness, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Render scale", &g_scale, 0.50f, 1.0f, "%.2f");
        ImGui::Separator();
        if (g_gen_present_allowed) {
            ImGui::Checkbox("Frame generation  (F5)", &g_generated_present_enabled);
            int mult_idx = (int)g_gen_mult - 2;
            const char* mults[] = { "2x", "3x", "4x" };
            if (ImGui::Combo("FG multiplier", &mult_idx, mults, 3)) {
                g_gen_mult = (unsigned)(mult_idx + 2);
                LOGF("[overlay-dx12] frame-generation multiplier set to %ux", g_gen_mult);
            }
        } else {
            g_generated_present_enabled = false;
            ImGui::TextDisabled("Frame generation: off (waitable swapchain; force via INI)");
        }
        ImGui::Checkbox("Motion interpolation preview  (F4)", &g_interpolation_enabled);
        ImGui::TextDisabled(g_history_ready ? "History: ready" : "History: warming up");
        ImGui::Separator();
        if (ImGui::Button("Quality"))     { g_scale = 0.85f; g_sharpness = 0.30f; } ImGui::SameLine();
        if (ImGui::Button("Balanced"))    { g_scale = 0.77f; g_sharpness = 0.50f; } ImGui::SameLine();
        if (ImGui::Button("Performance")) { g_scale = 0.67f; g_sharpness = 0.65f; }
        ImGui::Separator();
        ImGui::TextDisabled("Home: show/hide   keys still work alongside the mouse");
        ImGui::End();
    }

    // Build the ImGui draw data once per real frame (only when the menu is visible).
    bool imgui_begin_frame() {
        if (!g_imgui_ready || !g_menu_visible) return false;
        // Skip while minimized / zero-size: ImGui_ImplWin32_NewFrame() would report a
        // 0x0 display size, which can trip asserts in NewFrame()/RenderDrawData.
        if (g_hwnd) {
            if (IsIconic(g_hwnd)) return false;
            RECT rc{};
            if (GetClientRect(g_hwnd, &rc) && (rc.right <= rc.left || rc.bottom <= rc.top))
                return false;
        }
        ImGui::GetIO().MouseDrawCursor = true;   // always-visible software cursor
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        imgui_build_menu();
        ImGui::Render();
        ClipCursor(nullptr);   // unclip so the cursor is reachable even in fullscreen games
        return true;
    }

    void imgui_render_to(D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
        if (!g_imgui_ready) return;
        ImDrawData* dd = ImGui::GetDrawData();
        if (!dd || dd->CmdListsCount == 0) return;
        g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = { g_imgui_srv_heap };
        g_cmd->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(dd, g_cmd);
    }

    bool render_upscale(FrameContext& f) {
        if (!g_input || !g_lowres || !g_srv_heap || !g_downscale_pso || !g_easu_rcas_pso || !g_root_sig) return false;
        if (g_width == 0 || g_height == 0 || g_low_width == 0 || g_low_height == 0) return false;
        // Everything below records into g_cmd (our upscale passes + ImGui). Mark the
        // thread so the generic scout ignores our own command-list activity instead of
        // profiling it / re-entering through the global D3D12 vtable hooks.
        struct ScoutGuard {
            ScoutGuard()  { capture::scout::set_overlay_active(true); }
            ~ScoutGuard() { capture::scout::set_overlay_active(false); }
        } scout_guard;
        update_scout_motion_binding();
        const bool imgui_frame = imgui_begin_frame();              // ImGui path: build menu draw data
        const bool shader_menu = g_menu_visible && !g_use_imgui;   // legacy bitmap UI only as fallback

        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(g_cmd, g_input, g_input_state, D3D12_RESOURCE_STATE_COPY_DEST);
        g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_cmd->CopyResource(g_input, f.backbuffer);
        transition(g_cmd, g_input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_input_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        float final_inv_x = 1.0f / static_cast<float>(g_width);
        float final_inv_y = 1.0f / static_cast<float>(g_height);

        if (g_effect_enabled) {
            transition(g_cmd, g_lowres, g_lowres_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g_lowres_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            bind_fullscreen_state(g_lowres_rtv, g_low_width, g_low_height, g_downscale_pso,
                                  1.0f / static_cast<float>(g_width), 1.0f / static_cast<float>(g_height),
                                  g_sharpness, g_scale, false, true, g_history_ready, g_interpolation_enabled, g_generated_present_enabled,
                                  g_real_fps, g_output_fps, g_scout_motion_bound ? 1.0f : 0.0f, 0.0f, g_scout_motion_copy_ready ? 1.0f : 0.0f);
            transition(g_cmd, g_lowres, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_lowres_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            final_inv_x = 1.0f / static_cast<float>(g_low_width);
            final_inv_y = 1.0f / static_cast<float>(g_low_height);
        }

        // Real FidelityFX FSR 1.0: EASU reconstructs low-res -> full-res into
        // g_easu here; the composite passes below then run genuine RCAS on it.
        const bool fsr_active = g_fsr_real && g_easu && g_fsr_easu_pso && g_effect_enabled;
        if (fsr_active) {
            if (g_rcas_con_for_sharpness != g_sharpness) {
                fsr1::rcas_constants(g_rcas_con, fsr1::slider_to_stops(g_sharpness));
                g_rcas_con_for_sharpness = g_sharpness;
            }
            transition(g_cmd, g_easu, g_easu_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g_easu_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            bind_easu_pass();
            transition(g_cmd, g_easu, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_easu_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        if (g_generated_present_enabled && g_history_ready) {
            const unsigned frames_to_gen = (g_gen_mult >= 2 ? g_gen_mult : 2) - 1;
            unsigned rendered = 0;
            for (unsigned k = 0; k < frames_to_gen && k < kMaxGenFrames; ++k) {
                if (!g_gen[k]) break;
                const float t = static_cast<float>(k + 1) / static_cast<float>(g_gen_mult);
                transition(g_cmd, g_gen[k], g_gen_state[k], D3D12_RESOURCE_STATE_RENDER_TARGET);
                g_gen_state[k] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bind_fullscreen_state(g_gen_rtv[k], g_width, g_height, g_easu_rcas_pso,
                                      final_inv_x, final_inv_y,
                                      g_sharpness, g_scale, shader_menu, g_effect_enabled,
                                      g_history_ready, true, g_generated_present_enabled,
                                      g_real_fps, g_output_fps, g_scout_motion_bound ? 1.0f : 0.0f, 0.0f, g_scout_motion_copy_ready ? 1.0f : 0.0f,
                                      fsr_active, t);
                if (imgui_frame) imgui_render_to(g_gen_rtv[k]);   // menu on generated frames too (no flicker)
                transition(g_cmd, g_gen[k], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                g_gen_state[k] = D3D12_RESOURCE_STATE_COPY_SOURCE;
                ++rendered;
            }
            g_gen_ready = rendered;
        } else {
            g_gen_ready = 0;
        }

        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        bind_fullscreen_state(f.rtv, g_width, g_height, g_easu_rcas_pso,
                              final_inv_x, final_inv_y,
                              g_sharpness, g_scale, shader_menu, g_effect_enabled,
                              g_history_ready, g_interpolation_enabled, g_generated_present_enabled,
                              g_real_fps, g_output_fps, g_scout_motion_bound ? 1.0f : 0.0f, 0.0f, g_scout_motion_copy_ready ? 1.0f : 0.0f,
                              fsr_active);
        transition(g_cmd, g_input, g_input_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_input_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        transition(g_cmd, g_history, g_history_state, D3D12_RESOURCE_STATE_COPY_DEST);
        g_history_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_cmd->CopyResource(g_history, g_input);
        transition(g_cmd, g_history, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_history_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_history_ready = true;
        capture::scout::note_dx12_history(true);

        if (imgui_frame) imgui_render_to(f.rtv);   // draw the menu last, into the live backbuffer
        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return true;
    }

}

void handle_mouse_controls() {
    if (g_use_imgui && g_imgui_ready) return;   // ImGui owns the mouse via the WndProc hook
    if (!g_menu_visible || !g_hwnd) {
        g_prev_left_mouse = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        return;
    }
    POINT pt{};
    if (!GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt)) return;
    const bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool clicked = left_down && !g_prev_left_mouse;
    g_prev_left_mouse = left_down;
    if (!clicked) return;

    const int x = pt.x;
    const int y = pt.y;
    auto inside = [&](int x0, int y0, int x1, int y1) -> bool {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    };
    auto clamp01 = [](float v) -> float { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    auto clamp_scale = [](float v) -> float { return v < 0.50f ? 0.50f : (v > 1.00f ? 1.00f : v); };

    bool recreate_scale_resources = false;
    if (inside(20, 44, 245, 62)) {
        if (g_effect_allowed) {
            g_effect_enabled = !g_effect_enabled;
            LOGF("[overlay-dx12] mouse: DX12 post-process %s", g_effect_enabled ? "enabled" : "disabled");
        } else {
            LOGF("[overlay-dx12] mouse ignored: DX12 post-process disabled by FSRINJ_DX12_SHARPEN=0");
        }
    } else if (inside(98, 72, 226, 78)) {
        const float t = clamp01((static_cast<float>(x) - 98.0f) / 128.0f);
        g_scale = clamp_scale(0.50f + t * 0.50f);
        recreate_scale_resources = true;
        LOGF("[overlay-dx12] mouse: scale=%.2f", g_scale);
    } else if (inside(98, 94, 226, 100)) {
        g_sharpness = clamp01((static_cast<float>(x) - 98.0f) / 128.0f);
        core::config().sharpness.store(g_sharpness);
        LOGF("[overlay-dx12] mouse: sharpness=%.2f", g_sharpness);
    } else if (inside(20, 112, 245, 132)) {
        g_interpolation_enabled = !g_interpolation_enabled;
        LOGF("[overlay-dx12] mouse: motion interpolation %s (history=%s)", g_interpolation_enabled ? "enabled" : "disabled", g_history_ready ? "ready" : "warming");
    } else if (inside(20, 136, 245, 156)) {
        g_generated_present_enabled = !g_generated_present_enabled;
        g_gen_ready = 0;
        g_generated_present_log_count = 0;
        LOGF("[overlay-dx12] mouse: experimental generated-frame presentation %s (history=%s)", g_generated_present_enabled ? "enabled" : "disabled", g_history_ready ? "ready" : "warming");
    } else if (inside(20, 160, 245, 180)) {
        g_scout_motion_enabled = !g_scout_motion_enabled;
        g_gen_ready = 0;
        LOGF("[overlay-dx12] mouse: scout motion-vector candidate %s (bound=%s)", g_scout_motion_enabled ? "enabled" : "disabled", g_scout_motion_bound ? "yes" : "no");
    }

    if (recreate_scale_resources && g_init) {
        wait_for_gpu_idle();
        release_frame_resources();
        if (!create_render_targets(g_sc3)) {
            LOGF("[overlay-dx12] mouse: failed to recreate resources after scale change; disabling effect for safety");
            g_effect_enabled = false;
            return;
        }
        g_present_count = 0;
        LOGF("[overlay-dx12] mouse: recreated resources for scale=%.2f lowres=%ux%u", g_scale, g_low_width, g_low_height);
    }
}

bool on_present(IDXGISwapChain* sc) {
    if (g_device_lost) return true; // fully passive after device removal
    if (!g_init) {
        if (!init(sc)) return false;
        g_init = true;
        g_present_count = 0;
        LOGF("[overlay-dx12] init-only present skipped; FSR1-style upscale begins after warmup");
        return true;
    }

    update_fps_counters(0);

    ++g_present_count;
    if ((g_effect_enabled || g_menu_visible || g_generated_present_enabled) && g_present_count <= kWarmupPresents) {
        LOGF("[overlay-dx12] warmup present %u/%u; skipping FSR1-style upscale", g_present_count, kWarmupPresents);
        return true;
    }

    bool recreate_scale_resources = false;
    auto clamp01 = [](float v) -> float { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    auto clamp_scale = [](float v) -> float { return v < 0.50f ? 0.50f : (v > 1.00f ? 1.00f : v); };
    // Only act on hotkeys while the game window has focus. Edge state still
    // updates while unfocused (inside key_edge) so refocusing can't replay keys.
    const bool win_focused = !g_hwnd || GetForegroundWindow() == g_hwnd;
    auto pressed = [&](int vk) -> bool { const bool e = key_edge(vk); return win_focused && e; };

    if (pressed(core::config().toggle_key.load())) {
        g_menu_visible = !g_menu_visible;
        core::config().overlay_visible.store(g_menu_visible);
        LOGF("[overlay-dx12] Home: native menu %s", g_menu_visible ? "shown" : "hidden");
    }
    if (pressed(VK_END)) {
        if (g_effect_allowed) {
            g_effect_enabled = !g_effect_enabled;
            LOGF("[overlay-dx12] End: DX12 post-process %s", g_effect_enabled ? "enabled" : "disabled");
        } else {
            LOGF("[overlay-dx12] End ignored: DX12 post-process disabled by FSRINJ_DX12_SHARPEN=0");
        }
    }
    if (pressed(VK_PRIOR)) { // PageUp
        g_sharpness = clamp01(g_sharpness + 0.05f);
        core::config().sharpness.store(g_sharpness);
        LOGF("[overlay-dx12] PageUp: sharpness=%.2f", g_sharpness);
    }
    if (pressed(VK_NEXT)) { // PageDown
        g_sharpness = clamp01(g_sharpness - 0.05f);
        core::config().sharpness.store(g_sharpness);
        LOGF("[overlay-dx12] PageDown: sharpness=%.2f", g_sharpness);
    }
    if (pressed(VK_INSERT)) {
        g_scale = clamp_scale(g_scale + 0.02f);
        recreate_scale_resources = true;
        LOGF("[overlay-dx12] Insert: scale=%.2f", g_scale);
    }
    if (pressed(VK_DELETE)) {
        g_scale = clamp_scale(g_scale - 0.02f);
        recreate_scale_resources = true;
        LOGF("[overlay-dx12] Delete: scale=%.2f", g_scale);
    }
    if (pressed(VK_F1)) { g_scale = 0.77f; g_sharpness = 0.35f; recreate_scale_resources = true; LOGF("[overlay-dx12] F1 preset: Quality scale=%.2f sharpness=%.2f", g_scale, g_sharpness); }
    if (pressed(VK_F2)) { g_scale = 0.67f; g_sharpness = 0.45f; recreate_scale_resources = true; LOGF("[overlay-dx12] F2 preset: Balanced scale=%.2f sharpness=%.2f", g_scale, g_sharpness); }
    if (pressed(VK_F3)) { g_scale = 0.59f; g_sharpness = 0.55f; recreate_scale_resources = true; LOGF("[overlay-dx12] F3 preset: Performance scale=%.2f sharpness=%.2f", g_scale, g_sharpness); }
    if (pressed(VK_F4)) { g_interpolation_enabled = !g_interpolation_enabled; LOGF("[overlay-dx12] F4: motion interpolation %s (history=%s)", g_interpolation_enabled ? "enabled" : "disabled", g_history_ready ? "ready" : "warming"); }
    if (pressed(VK_F5)) {
        if (!g_gen_present_allowed) {
            LOGF("[overlay-dx12] F5 ignored: waitable swapchain; set FSRINJ_DX12_GENPRESENT_FORCE=1 in fsrinj.ini to test anyway");
        } else {
            g_generated_present_enabled = !g_generated_present_enabled; g_gen_ready = 0; g_generated_present_log_count = 0;
            LOGF("[overlay-dx12] F5: experimental generated-frame presentation %s (history=%s)", g_generated_present_enabled ? "enabled" : "disabled", g_history_ready ? "ready" : "warming");
        }
    }
    if (pressed(VK_F8)) {
        // Cycle 2x -> 3x -> 4x. Takes effect on the next rendered frame; any
        // already-rendered generated frames this cycle are dropped so the
        // temporal spacing never mixes multipliers.
        g_gen_mult = g_gen_mult >= 4 ? 2 : g_gen_mult + 1;
        g_gen_ready = 0;
        LOGF("[overlay-dx12] F8: frame-generation multiplier %ux (%u generated frame(s) per real frame)",
             g_gen_mult, g_gen_mult - 1);
    }
    if (pressed(VK_F6)) {
        g_scout_motion_enabled = !g_scout_motion_enabled;
        g_scout_motion_use_enabled = false;
        g_scout_motion_copy_requested = false;
        g_scout_motion_bound = false;
        g_scout_motion_copy_ready = false;
        g_scout_motion_create_only_logged = false;
        safe_release(g_scout_motion);
        g_scout_motion_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_scout_motion_width = 0;
        g_scout_motion_height = 0;
        g_scout_motion_format = DXGI_FORMAT_UNKNOWN;
        g_gen_ready = 0;
        LOGF("[overlay-dx12] F6: scout MV validation %s; stage1=create only, F7=copy/use", g_scout_motion_enabled ? "enabled" : "disabled");
    }
    if (pressed(VK_F7)) {
        if (!g_scout_motion_enabled) {
            LOGF("[overlay-dx12] F7 ignored: enable F6 scout MV validation first");
        } else if (!g_scout_motion_copy_ready) {
            g_scout_motion_copy_requested = true;
            g_scout_motion_use_enabled = false;
            g_scout_motion_bound = false;
            LOGF("[overlay-dx12] F7: scout MV copy validation requested; shader use still disabled");
        } else {
            g_scout_motion_use_enabled = !g_scout_motion_use_enabled;
            g_gen_ready = 0;
            LOGF("[overlay-dx12] F7: scout MV shader use %s (copy=ready)", g_scout_motion_use_enabled ? "enabled" : "disabled");
        }
    }

    handle_mouse_controls();

    if (recreate_scale_resources && g_init) {
        wait_for_gpu_idle();
        release_frame_resources();
        if (!create_render_targets(sc)) {
            LOGF("[overlay-dx12] failed to recreate resources after scale change; disabling effect for safety");
            g_effect_enabled = false;
            return true;
        }
        g_present_count = 0;
        g_logged_first_effect = false;
        LOGF("[overlay-dx12] recreated resources for scale=%.2f lowres=%ux%u", g_scale, g_low_width, g_low_height);
        return true;
    }

    if (!g_effect_enabled && !g_menu_visible && !g_generated_present_enabled) return true;

    const UINT idx = g_sc3 ? g_sc3->GetCurrentBackBufferIndex() : 0;
    if (idx >= g_frames.size()) {
        LOGF("[overlay-dx12] invalid backbuffer index %u size=%u", idx, (unsigned)g_frames.size());
        return true;
    }
    FrameContext& f = g_frames[idx];
    if (!f.allocator || !f.backbuffer || !g_cmd || !g_queue) return true;
    if (!wait_for_frame(f)) return true;

    HRESULT hr = f.allocator->Reset();
    if (FAILED(hr)) {
        LOGF("[overlay-dx12] allocator Reset failed hr=0x%08lX", hr);
        return true;
    }
    hr = g_cmd->Reset(f.allocator, nullptr);
    if (FAILED(hr)) {
        LOGF("[overlay-dx12] command list Reset failed hr=0x%08lX; recovering", hr);
        recover_command_list();
        return true;
    }

    if (!render_upscale(f)) {
        LOGF("[overlay-dx12] render_upscale returned false; skipping frame");
        if (FAILED(g_cmd->Close())) recover_command_list();
        return true;
    }

    hr = g_cmd->Close();
    if (FAILED(hr)) {
        LOGF("[overlay-dx12] command list Close failed hr=0x%08lX; recovering", hr);
        recover_command_list();
        return true;
    }

    ID3D12CommandList* lists[] = { g_cmd };
    g_queue->ExecuteCommandLists(1, lists);
    signal_frame(f);
    if (!g_logged_first_effect) {
        LOGF("[overlay-dx12] first DX12 FSR1-style EASU/RCAS + native UI frame submitted successfully");
        g_logged_first_effect = true;
    }
    return true;
}

void on_resize_buffers() { release_frame_resources(); }
void on_after_resize(IDXGISwapChain* sc) {
    if (g_init && g_dev) {
        if (!create_render_targets(sc)) LOGF("[overlay-dx12] recreate EASU-style test upscale resources failed after ResizeBuffers");
        else {
            g_present_count = 0;
            g_logged_first_effect = false;
            LOGF("[overlay-dx12] FSR1-style upscale resources recreated after ResizeBuffers");
        }
    }
}


namespace {
    bool submit_generated_to_current_backbuffer(IDXGISwapChain* sc, unsigned k) {
        if (g_device_lost || !g_gen_present_allowed) return false;
        if (!g_init || !g_sc3 || !g_generated_present_enabled || g_gen_ready == 0 || g_inside_generated_present) return false;
        if (k >= kMaxGenFrames || !g_gen[k]) return false;
        if (!g_cmd || !g_queue || g_frames.empty()) return false;
        const UINT idx = g_sc3->GetCurrentBackBufferIndex();
        if (idx >= g_frames.size()) return false;
        FrameContext& f = g_frames[idx];
        if (!f.allocator || !f.backbuffer) return false;
        if (!wait_for_frame(f)) return false;

        HRESULT hr = f.allocator->Reset();
        if (FAILED(hr)) { LOGF("[overlay-dx12] generated-present allocator Reset failed hr=0x%08lX", hr); return false; }
        hr = g_cmd->Reset(f.allocator, nullptr);
        if (FAILED(hr)) { LOGF("[overlay-dx12] generated-present command list Reset failed hr=0x%08lX; recovering", hr); recover_command_list(); return false; }

        // Our own blit of the generated frame -- keep it out of the scout's view.
        struct ScoutGuard {
            ScoutGuard()  { capture::scout::set_overlay_active(true); }
            ~ScoutGuard() { capture::scout::set_overlay_active(false); }
        } scout_guard;

        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(g_cmd, g_gen[k], g_gen_state[k], D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_gen_state[k] = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_cmd->CopyResource(f.backbuffer, g_gen[k]);
        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);

        hr = g_cmd->Close();
        if (FAILED(hr)) { LOGF("[overlay-dx12] generated-present command list Close failed hr=0x%08lX; recovering", hr); recover_command_list(); return false; }
        ID3D12CommandList* lists[] = { g_cmd };
        g_queue->ExecuteCommandLists(1, lists);
        signal_frame(f);
        return true;
    }

    // Record when each real frame was presented and keep a smoothed estimate of the
    // gap between real presents. Implausible gaps (alt-tab, hitches, first frame) are
    // ignored so one stutter doesn't poison the average.
    void note_real_present_timing() {
        if (g_qpc_freq.QuadPart == 0) QueryPerformanceFrequency(&g_qpc_freq);
        if (g_qpc_freq.QuadPart == 0) return;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (g_last_real_present_qpc.QuadPart != 0) {
            double dt = double(now.QuadPart - g_last_real_present_qpc.QuadPart) / double(g_qpc_freq.QuadPart);
            if (dt > 0.0005 && dt < 0.2) {  // 5000fps..5fps window
                if (g_real_interval_sec <= 0.0) g_real_interval_sec = dt;
                else g_real_interval_sec = g_real_interval_sec * 0.9 + dt * 0.1;
            }
        }
        g_last_real_present_qpc = now;
    }

    // Hold the generated frame back so it lands ~halfway (in time) between the two real
    // frames instead of right on top of the previous one. Without this, doubling the
    // present count still judders because frames arrive in bursts. Capped so a hitch
    // can never stall the game thread for long. This costs up to ~half a frame of
    // latency on the generated frame; a dedicated present thread would remove that.
    void pace_generated_present(double fraction) {
        if (!g_pacing_enabled || g_real_interval_sec <= 0.0 || g_qpc_freq.QuadPart == 0) return;
        double target = fraction * g_real_interval_sec;
        double cap = g_real_interval_sec * 0.9;
        if (target > cap) target = cap;
        if (target <= 0.0) return;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        double elapsed = double(now.QuadPart - g_last_real_present_qpc.QuadPart) / double(g_qpc_freq.QuadPart);
        double remain = target - elapsed;
        if (remain <= 0.0) return;
        // Bulk of the wait on a high-resolution waitable timer (no CPU burn), leaving a
        // ~0.4ms margin to finish with a short spin for precision. Falls back to the spin
        // entirely if the timer was unavailable.
        if (g_pace_timer && remain > 0.0006) {
            LARGE_INTEGER due{}; due.QuadPart = -(LONGLONG)((remain - 0.0004) * 1.0e7); // 100ns units, relative
            if (SetWaitableTimerEx(g_pace_timer, &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject(g_pace_timer, 8);
        }
        int guard = 0;
        for (;;) {
            QueryPerformanceCounter(&now);
            elapsed = double(now.QuadPart - g_last_real_present_qpc.QuadPart) / double(g_qpc_freq.QuadPart);
            if (elapsed >= target || ++guard > 500000) break;
            YieldProcessor();
        }
    }
}

void after_present(IDXGISwapChain* sc, unsigned int flags, PresentFn present_fn) {
    note_real_present_timing();
    if (!present_fn || g_gen_ready == 0) return;
    const unsigned count = g_gen_ready;
    const unsigned mult = count + 1;
    for (unsigned k = 0; k < count; ++k) {
        if (!submit_generated_to_current_backbuffer(sc, k)) break;
        // 2x keeps the user-tunable midpoint; 3x/4x space frames evenly at k/N.
        pace_generated_present(mult == 2 ? g_pace_fraction
                                         : static_cast<double>(k + 1) / static_cast<double>(mult));
        g_inside_generated_present = true;
        HRESULT hr = present_fn(sc, 0, flags);
        g_inside_generated_present = false;
        if (FAILED(hr)) { LOGF("[overlay-dx12] generated Present failed hr=0x%08lX", hr); break; }
        update_fps_counters(1);
        ++g_generated_present_log_count;
        if (g_generated_present_log_count == 1 || (g_generated_present_log_count % 120u) == 0u)
            LOGF("[overlay-dx12] experimental generated frame presented count=%u (x%u mode)", g_generated_present_log_count, mult);
    }
    g_gen_ready = 0;
}

void after_present1(IDXGISwapChain1* sc, unsigned int flags, const DXGI_PRESENT_PARAMETERS* pp, Present1Fn present1_fn) {
    note_real_present_timing();
    if (!present1_fn || g_gen_ready == 0) return;
    DXGI_PRESENT_PARAMETERS empty{};
    const DXGI_PRESENT_PARAMETERS* use_pp = pp ? pp : &empty;
    const unsigned count = g_gen_ready;
    const unsigned mult = count + 1;
    for (unsigned k = 0; k < count; ++k) {
        if (!submit_generated_to_current_backbuffer(reinterpret_cast<IDXGISwapChain*>(sc), k)) break;
        pace_generated_present(mult == 2 ? g_pace_fraction
                                         : static_cast<double>(k + 1) / static_cast<double>(mult));
        g_inside_generated_present = true;
        HRESULT hr = present1_fn(sc, 0, flags, use_pp);
        g_inside_generated_present = false;
        if (FAILED(hr)) { LOGF("[overlay-dx12] generated Present1 failed hr=0x%08lX", hr); break; }
        update_fps_counters(1);
        ++g_generated_present_log_count;
        if (g_generated_present_log_count == 1 || (g_generated_present_log_count % 120u) == 0u)
            LOGF("[overlay-dx12] experimental generated frame presented via Present1 count=%u (x%u mode)", g_generated_present_log_count, mult);
    }
    g_gen_ready = 0;
}

void shutdown() {
    if (g_init) wait_for_gpu_idle();
    imgui_shutdown();
    release_frame_resources();
    if (g_fence_event) { CloseHandle(g_fence_event); g_fence_event = nullptr; }
    safe_release(g_fence);
    safe_release(g_queue);
    safe_release(g_dev);
    safe_release(g_sc3);
    g_next_fence_value = 1;
    g_effect_allowed = true;
    g_effect_enabled = true;
    g_menu_visible = true;
    g_logged_first_effect = false;
    g_last_real_present_qpc = LARGE_INTEGER{};
    g_real_interval_sec = 0.0;
    if (g_pace_timer) { CloseHandle(g_pace_timer); g_pace_timer = nullptr; }
    g_history_ready = false;
    g_interpolation_enabled = false;
    g_generated_present_enabled = false;
    g_gen_ready = 0;
    g_inside_generated_present = false;
    g_prev_left_mouse = false;
    g_qpc_freq = LARGE_INTEGER{};
    g_fps_window_start = LARGE_INTEGER{};
    g_real_present_samples = 0;
    g_generated_present_samples = 0;
    g_generated_present_log_count = 0;
    g_real_fps = 0.0f;
    g_output_fps = 0.0f;
    g_sharpness = 0.20f;
    g_scale = 0.77f;
    g_present_count = 0;
    g_init = false;
}

} // namespace overlay::dx12
