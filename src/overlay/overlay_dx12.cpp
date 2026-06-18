#include "overlay/overlay_dx12.h"
#include "hooks/dx12_queue_capture.h"
#include "core/config.h"
#include "core/log.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <dxgi1_2.h>
#include <cstring>
#include <cwchar>
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
    ID3D12DescriptorHeap* g_srv_heap = nullptr;
    ID3D12GraphicsCommandList* g_cmd = nullptr;
    ID3D12Fence* g_fence = nullptr;
    ID3D12RootSignature* g_root_sig = nullptr;
    ID3D12PipelineState* g_downscale_pso = nullptr;
    ID3D12PipelineState* g_easu_rcas_pso = nullptr;
    ID3D12Resource* g_input = nullptr;
    ID3D12Resource* g_lowres = nullptr;
    ID3D12Resource* g_history = nullptr;
    ID3D12Resource* g_generated = nullptr;
    HANDLE g_fence_event = nullptr;
    UINT64 g_next_fence_value = 1;
    std::vector<FrameContext> g_frames;
    UINT g_rtv_stride = 0;
    UINT g_srv_stride = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE g_lowres_rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE g_generated_rtv{};
    DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT g_width = 0;
    UINT g_height = 0;
    HWND g_hwnd = nullptr;
    bool g_effect_allowed = true;
    bool g_effect_enabled = true;
    bool g_menu_visible = true;
    bool g_logged_first_effect = false;
    bool g_history_ready = false;
    bool g_interpolation_enabled = false;
    bool g_generated_present_enabled = false;
    bool g_generated_ready = false;
    bool g_prev_left_mouse = false;
    bool g_inside_generated_present = false;
    float g_sharpness = 0.20f;
    float g_scale = 0.77f;
    UINT g_low_width = 0;
    UINT g_low_height = 0;
    unsigned g_present_count = 0;
    const unsigned kWarmupPresents = 3;
    D3D12_RESOURCE_STATES g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES g_lowres_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES g_history_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES g_generated_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

    template <class T>
    void safe_release(T*& p) {
        if (p) { p->Release(); p = nullptr; }
    }

    bool env_disabled(const wchar_t* name) {
        wchar_t value[16]{};
        DWORD n = GetEnvironmentVariableW(name, value, 16);
        if (n == 0 || n >= 16) return false;
        return value[0] == L'0' || value[0] == L'n' || value[0] == L'N' ||
               value[0] == L'f' || value[0] == L'F';
    }

    float env_float(const wchar_t* name, float fallback) {
        wchar_t value[64]{};
        DWORD n = GetEnvironmentVariableW(name, value, 64);
        if (n == 0 || n >= 64) return fallback;
        wchar_t* end = nullptr;
        float parsed = std::wcstof(value, &end);
        if (end == value) return fallback;
        if (parsed < 0.0f) parsed = 0.0f;
        if (parsed > 1.0f) parsed = 1.0f;
        return parsed;
    }

    float env_scale(const wchar_t* name, float fallback) {
        float parsed = env_float(name, fallback);
        if (parsed < 0.50f) parsed = 0.50f;
        if (parsed > 1.00f) parsed = 1.00f;
        return parsed;
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
        WaitForSingleObject(g_fence_event, 2000);
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

    bool compile_shader(const char* source, const char* entry, const char* target, ID3DBlob** blob) {
        UINT flags = 0;
    #if defined(_DEBUG)
        flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    #endif
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                entry, target, flags, 0, blob, &errors);
        if (FAILED(hr)) {
            if (errors) {
                LOGF("[overlay-dx12] D3DCompile %s/%s failed hr=0x%08lX: %s", entry, target, hr,
                     static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            } else {
                LOGF("[overlay-dx12] D3DCompile %s/%s failed hr=0x%08lX", entry, target, hr);
            }
            return false;
        }
        if (errors) errors->Release();
        return true;
    }

    bool create_upscale_pipeline() {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 3;
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
        params[1].Constants.Num32BitValues = 11;
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

        const char* hlsl = R"HLSL(
Texture2D<float4> gInput : register(t0);
Texture2D<float4> gHistory : register(t1);
Texture2D<float4> gLowres : register(t2);
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
};
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

float3 draw_native_ui(float3 color, float2 pos) {
    if (overlayOn < 0.5) return color;
    float2 panelMin = float2(14.0, 14.0);
    float2 panelMax = float2(286.0, 172.0);
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
    if (interpOn > 0.5) white = max(white, text_pixel(70,52,32,80,82,69,86,32,79,78,32,32, pos, o + float2(0, 90), 2.0)); // F4 PREV ON
    else white = max(white, text_pixel(70,52,32,80,82,69,86,32,79,70,70,32, pos, o + float2(0, 90), 2.0)); // F4 PREV OFF
    if (genPresentOn > 0.5) white = max(white, text_pixel(70,53,32,71,69,78,32,79,78,32,32,32, pos, o + float2(0, 114), 2.0)); // F5 GEN ON
    else white = max(white, text_pixel(70,53,32,71,69,78,32,79,70,70,32,32, pos, o + float2(0, 114), 2.0)); // F5 GEN OFF
    if (historyReady > 0.5) white = max(white, text_pixel(72,73,83,84,32,82,69,65,68,89,32,32, pos, o + float2(0, 138), 2.0)); // HIST READY
    color = lerp(color, float3(0.92, 0.92, 0.95), white);

    float scaleBg = step(98.0, pos.x) * step(72.0, pos.y) * step(pos.x, 226.0) * step(pos.y, 78.0);
    float scaleFill = step(98.0, pos.x) * step(72.0, pos.y) * step(pos.x, 98.0 + 128.0 * saturate((scale - 0.50) / 0.50)) * step(pos.y, 78.0);
    float sharpBg = step(98.0, pos.x) * step(94.0, pos.y) * step(pos.x, 226.0) * step(pos.y, 100.0);
    float sharpFill = step(98.0, pos.x) * step(94.0, pos.y) * step(pos.x, 98.0 + 128.0 * saturate(sharpness)) * step(pos.y, 100.0);
    color = lerp(color, float3(0.10, 0.10, 0.12), (scaleBg + sharpBg) * 0.80);
    color = lerp(color, float3(0.95, 0.18, 0.95), (scaleFill + sharpFill) * 0.80);
    return color;
}

float4 EasuRcasPS(VSOut i) : SV_Target {
    float4 base = gInput.SampleLevel(gSampler, i.uv, 0.0);
    float3 color = base.rgb;
    if (effectOn > 0.5) {
        float3 up = fsr1_easu(i.uv);
        float3 sharp = fsr1_rcas(i.uv, invSize, sharpness);
        float amount = saturate(0.45 + sharpness * 0.55);
        color = saturate(lerp(up, sharp, amount));
    }
    if (interpOn > 0.5 && historyReady > 0.5) {
        float3 prev = gHistory.SampleLevel(gSampler, i.uv, 0.0).rgb;
        // First experimental interpolation mode. This does not yet insert extra
        // Presents; it validates the frame-history path by blending the current
        // processed frame with the previous captured frame.
        color = saturate(lerp(prev, color, 0.55));
    }
    color = draw_native_ui(color, i.pos.xy);
    return float4(color, base.a);
}
)HLSL";

        ID3DBlob* vs = nullptr;
        ID3DBlob* ps_down = nullptr;
        ID3DBlob* ps_easu = nullptr;
        if (!compile_shader(hlsl, "VSMain", "vs_5_0", &vs)) return false;
        if (!compile_shader(hlsl, "DownscalePS", "ps_5_0", &ps_down)) { vs->Release(); return false; }
        if (!compile_shader(hlsl, "EasuRcasPS", "ps_5_0", &ps_easu)) { vs->Release(); ps_down->Release(); return false; }

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
        vs->Release();
        ps_down->Release();
        ps_easu->Release();
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
        gen_clear.Color[0] = 0.0f;
        gen_clear.Color[1] = 0.0f;
        gen_clear.Color[2] = 0.0f;
        gen_clear.Color[3] = 1.0f;
        hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &gen_desc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &gen_clear,
                                            IID_PPV_ARGS(&g_generated));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateCommittedResource(generated texture) failed hr=0x%08lX %ux%u fmt=%u", hr, g_width, g_height, (unsigned)g_format);
            return false;
        }
        g_generated_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_history_ready = false;
        g_generated_ready = false;

        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors = 3;
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
        safe_release(g_generated);
        safe_release(g_srv_heap);
        safe_release(g_downscale_pso);
        safe_release(g_easu_rcas_pso);
        safe_release(g_root_sig);
        safe_release(g_rtv_heap);
        g_width = 0;
        g_height = 0;
        g_low_width = 0;
        g_low_height = 0;
        g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_lowres_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_history_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_generated_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_history_ready = false;
        g_generated_ready = false;
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
        rtv_desc.NumDescriptors = buffer_count + 2;
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
        g_generated_rtv = cpu;
        if (!create_upscale_resources_from_backbuffer(g_frames[0].backbuffer)) return false;
        g_dev->CreateRenderTargetView(g_lowres, nullptr, g_lowres_rtv);
        if (g_generated) g_dev->CreateRenderTargetView(g_generated, nullptr, g_generated_rtv);
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
        g_effect_allowed = !env_disabled(L"FSRINJ_DX12_SHARPEN");
        g_effect_enabled = g_effect_allowed;
        g_menu_visible = core::config().overlay_visible.load();
        g_sharpness = env_float(L"FSRINJ_DX12_SHARPNESS", core::config().sharpness.load());
        if (g_sharpness <= 0.0f) g_sharpness = 0.20f;
        if (g_sharpness > 1.0f) g_sharpness = 1.0f;
        g_scale = env_scale(L"FSRINJ_DX12_SCALE", 0.77f);
        g_interpolation_enabled = !env_disabled(L"FSRINJ_DX12_INTERP") && env_float(L"FSRINJ_DX12_INTERP", 0.0f) > 0.5f;
        g_generated_present_enabled = !env_disabled(L"FSRINJ_DX12_GENPRESENT") && env_float(L"FSRINJ_DX12_GENPRESENT", 0.0f) > 0.5f;

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

        LOGF("[overlay-dx12] FSR1-style EASU/RCAS + native UI initialized on hwnd %p buffers=%u size=%ux%u lowres=%ux%u scale=%.2f format=%u queue=%p enabled=%s sharpness=%.2f genpresent=%s",
             static_cast<void*>(g_hwnd), (unsigned)g_frames.size(), g_width, g_height, g_low_width, g_low_height, g_scale, (unsigned)g_format,
             static_cast<void*>(g_queue), g_effect_enabled ? "on" : "off", g_sharpness, g_generated_present_enabled ? "on" : "off");
        LOGF("[overlay-dx12] Native DX12 settings overlay is enabled; Home=menu End=effect PgUp/PgDn=sharpness Insert/Delete=scale F1/F2/F3=presets F4=preview-interp F5=generated-present; mouse clicks supported");
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
                               bool menu_visible, bool effect_enabled, bool history_ready, bool interp_enabled, bool gen_present_enabled) {
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
        const float params[11] = { inv_x, inv_y, sharpness, scale, 1.0f / static_cast<float>(g_width ? g_width : width), 1.0f / static_cast<float>(g_height ? g_height : height), menu_visible ? 1.0f : 0.0f, effect_enabled ? 1.0f : 0.0f, history_ready ? 1.0f : 0.0f, interp_enabled ? 1.0f : 0.0f, gen_present_enabled ? 1.0f : 0.0f };
        g_cmd->SetGraphicsRoot32BitConstants(1, 11, params, 0);
        g_cmd->DrawInstanced(3, 1, 0, 0);
    }

    bool render_upscale(FrameContext& f) {
        if (!g_input || !g_lowres || !g_srv_heap || !g_downscale_pso || !g_easu_rcas_pso || !g_root_sig) return false;
        if (g_width == 0 || g_height == 0 || g_low_width == 0 || g_low_height == 0) return false;

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
                                  g_sharpness, g_scale, false, true, g_history_ready, g_interpolation_enabled, g_generated_present_enabled);
            transition(g_cmd, g_lowres, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_lowres_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            final_inv_x = 1.0f / static_cast<float>(g_low_width);
            final_inv_y = 1.0f / static_cast<float>(g_low_height);
        }

        if (g_generated_present_enabled && g_generated && g_history_ready) {
            transition(g_cmd, g_generated, g_generated_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g_generated_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            bind_fullscreen_state(g_generated_rtv, g_width, g_height, g_easu_rcas_pso,
                                  final_inv_x, final_inv_y,
                                  g_sharpness, g_scale, false, g_effect_enabled,
                                  true, true, g_generated_present_enabled);
            transition(g_cmd, g_generated, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g_generated_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            g_generated_ready = true;
        } else {
            g_generated_ready = false;
        }

        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        bind_fullscreen_state(f.rtv, g_width, g_height, g_easu_rcas_pso,
                              final_inv_x, final_inv_y,
                              g_sharpness, g_scale, g_menu_visible, g_effect_enabled,
                              g_history_ready, g_interpolation_enabled, g_generated_present_enabled);
        transition(g_cmd, g_input, g_input_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_input_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        transition(g_cmd, g_history, g_history_state, D3D12_RESOURCE_STATE_COPY_DEST);
        g_history_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_cmd->CopyResource(g_history, g_input);
        transition(g_cmd, g_history, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_history_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_history_ready = true;

        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return true;
    }

}

void handle_mouse_controls() {
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
        LOGF("[overlay-dx12] mouse: preview interpolation %s (history=%s)", g_interpolation_enabled ? "enabled" : "disabled", g_history_ready ? "ready" : "warming");
    } else if (inside(20, 136, 245, 156)) {
        g_generated_present_enabled = !g_generated_present_enabled;
        g_generated_ready = false;
        LOGF("[overlay-dx12] mouse: experimental generated-frame presentation %s (history=%s)", g_generated_present_enabled ? "enabled" : "disabled", g_history_ready ? "ready" : "warming");
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
    if (!g_init) {
        if (!init(sc)) return false;
        g_init = true;
        g_present_count = 0;
        LOGF("[overlay-dx12] init-only present skipped; FSR1-style upscale begins after warmup");
        return true;
    }

    ++g_present_count;
    if ((g_effect_enabled || g_menu_visible || g_generated_present_enabled) && g_present_count <= kWarmupPresents) {
        LOGF("[overlay-dx12] warmup present %u/%u; skipping FSR1-style upscale", g_present_count, kWarmupPresents);
        return true;
    }

    bool recreate_scale_resources = false;
    auto clamp01 = [](float v) -> float { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    auto clamp_scale = [](float v) -> float { return v < 0.50f ? 0.50f : (v > 1.00f ? 1.00f : v); };
    auto pressed = [](int vk) -> bool { return (GetAsyncKeyState(vk) & 0x0001) != 0; };

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
    if (pressed(VK_F4)) { g_interpolation_enabled = !g_interpolation_enabled; LOGF("[overlay-dx12] F4: preview interpolation %s (history=%s)", g_interpolation_enabled ? "enabled" : "disabled", g_history_ready ? "ready" : "warming"); }
    if (pressed(VK_F5)) { g_generated_present_enabled = !g_generated_present_enabled; g_generated_ready = false; LOGF("[overlay-dx12] F5: experimental generated-frame presentation %s (history=%s)", g_generated_present_enabled ? "enabled" : "disabled", g_history_ready ? "ready" : "warming"); }

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
        LOGF("[overlay-dx12] command list Reset failed hr=0x%08lX", hr);
        return true;
    }

    if (!render_upscale(f)) {
        LOGF("[overlay-dx12] render_upscale returned false; skipping frame");
        g_cmd->Close();
        return true;
    }

    hr = g_cmd->Close();
    if (FAILED(hr)) {
        LOGF("[overlay-dx12] command list Close failed hr=0x%08lX", hr);
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
    bool submit_generated_to_current_backbuffer(IDXGISwapChain* sc) {
        if (!g_init || !g_sc3 || !g_generated_present_enabled || !g_generated_ready || !g_generated || g_inside_generated_present) return false;
        if (!g_cmd || !g_queue || g_frames.empty()) return false;
        const UINT idx = g_sc3->GetCurrentBackBufferIndex();
        if (idx >= g_frames.size()) return false;
        FrameContext& f = g_frames[idx];
        if (!f.allocator || !f.backbuffer) return false;
        if (!wait_for_frame(f)) return false;

        HRESULT hr = f.allocator->Reset();
        if (FAILED(hr)) { LOGF("[overlay-dx12] generated-present allocator Reset failed hr=0x%08lX", hr); return false; }
        hr = g_cmd->Reset(f.allocator, nullptr);
        if (FAILED(hr)) { LOGF("[overlay-dx12] generated-present command list Reset failed hr=0x%08lX", hr); return false; }

        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(g_cmd, g_generated, g_generated_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_generated_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_cmd->CopyResource(f.backbuffer, g_generated);
        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);

        hr = g_cmd->Close();
        if (FAILED(hr)) { LOGF("[overlay-dx12] generated-present command list Close failed hr=0x%08lX", hr); return false; }
        ID3D12CommandList* lists[] = { g_cmd };
        g_queue->ExecuteCommandLists(1, lists);
        signal_frame(f);
        g_generated_ready = false;
        return true;
    }
}

void after_present(IDXGISwapChain* sc, unsigned int flags, PresentFn present_fn) {
    if (!present_fn || !submit_generated_to_current_backbuffer(sc)) return;
    g_inside_generated_present = true;
    HRESULT hr = present_fn(sc, 0, flags);
    g_inside_generated_present = false;
    if (SUCCEEDED(hr)) LOGF("[overlay-dx12] experimental generated frame presented");
    else LOGF("[overlay-dx12] generated Present failed hr=0x%08lX", hr);
}

void after_present1(IDXGISwapChain1* sc, unsigned int flags, const DXGI_PRESENT_PARAMETERS* pp, Present1Fn present1_fn) {
    if (!present1_fn || !submit_generated_to_current_backbuffer(reinterpret_cast<IDXGISwapChain*>(sc))) return;
    g_inside_generated_present = true;
    DXGI_PRESENT_PARAMETERS empty{};
    const DXGI_PRESENT_PARAMETERS* use_pp = pp ? pp : &empty;
    HRESULT hr = present1_fn(sc, 0, flags, use_pp);
    g_inside_generated_present = false;
    if (SUCCEEDED(hr)) LOGF("[overlay-dx12] experimental generated frame presented via Present1");
    else LOGF("[overlay-dx12] generated Present1 failed hr=0x%08lX", hr);
}

void shutdown() {
    if (g_init) wait_for_gpu_idle();
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
    g_history_ready = false;
    g_interpolation_enabled = false;
    g_generated_present_enabled = false;
    g_generated_ready = false;
    g_inside_generated_present = false;
    g_prev_left_mouse = false;
    g_sharpness = 0.20f;
    g_scale = 0.77f;
    g_present_count = 0;
    g_init = false;
}

} // namespace overlay::dx12
