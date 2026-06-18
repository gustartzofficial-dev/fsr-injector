#include "overlay/overlay_dx12.h"
#include "hooks/dx12_queue_capture.h"
#include "core/config.h"
#include "core/log.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_4.h>
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
    ID3D12PipelineState* g_pso = nullptr;
    ID3D12Resource* g_input = nullptr;
    HANDLE g_fence_event = nullptr;
    UINT64 g_next_fence_value = 1;
    std::vector<FrameContext> g_frames;
    UINT g_rtv_stride = 0;
    DXGI_FORMAT g_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT g_width = 0;
    UINT g_height = 0;
    HWND g_hwnd = nullptr;
    bool g_sharpen_enabled = true;
    bool g_logged_first_effect = false;
    float g_sharpness = 0.20f;
    unsigned g_present_count = 0;
    const unsigned kWarmupPresents = 3;
    D3D12_RESOURCE_STATES g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;

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

    bool create_sharpen_pipeline() {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
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
        params[1].Constants.Num32BitValues = 4;
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
SamplerState gSampler : register(s0);
cbuffer Params : register(b0) {
    float2 invSize;
    float sharpness;
    float pad0;
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
float4 PSMain(VSOut i) : SV_Target {
    float2 px = invSize;
    float4 c = gInput.SampleLevel(gSampler, i.uv, 0.0);
    float4 l = gInput.SampleLevel(gSampler, i.uv + float2(-px.x, 0.0), 0.0);
    float4 r = gInput.SampleLevel(gSampler, i.uv + float2( px.x, 0.0), 0.0);
    float4 u = gInput.SampleLevel(gSampler, i.uv + float2(0.0, -px.y), 0.0);
    float4 d = gInput.SampleLevel(gSampler, i.uv + float2(0.0,  px.y), 0.0);
    float a = sharpness * 0.35;
    float4 outc = c * (1.0 + 4.0 * a) - (l + r + u + d) * a;
    outc.a = c.a;
    return saturate(outc);
}
)HLSL";

        ID3DBlob* vs = nullptr;
        ID3DBlob* ps = nullptr;
        if (!compile_shader(hlsl, "VSMain", "vs_5_0", &vs)) return false;
        if (!compile_shader(hlsl, "PSMain", "ps_5_0", &ps)) { vs->Release(); return false; }

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

        hr = g_dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pso));
        vs->Release();
        ps->Release();
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateGraphicsPipelineState failed hr=0x%08lX format=%u", hr, (unsigned)g_format);
            return false;
        }
        return true;
    }

    bool create_input_texture_from_backbuffer(ID3D12Resource* backbuffer) {
        if (!backbuffer) return false;
        D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
        g_width = static_cast<UINT>(desc.Width);
        g_height = desc.Height;
        g_format = desc.Format == DXGI_FORMAT_UNKNOWN ? g_format : desc.Format;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        HRESULT hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&g_input));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateCommittedResource(input texture) failed hr=0x%08lX %ux%u fmt=%u", hr, g_width, g_height, (unsigned)g_format);
            return false;
        }
        g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;

        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors = 1;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dev->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&g_srv_heap));
        if (FAILED(hr)) {
            LOGF("[overlay-dx12] CreateDescriptorHeap(SRV) failed hr=0x%08lX", hr);
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = g_format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        view.Texture2D.MostDetailedMip = 0;
        view.Texture2D.PlaneSlice = 0;
        view.Texture2D.ResourceMinLODClamp = 0.0f;
        g_dev->CreateShaderResourceView(g_input, &view, g_srv_heap->GetCPUDescriptorHandleForHeapStart());
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
        safe_release(g_srv_heap);
        safe_release(g_pso);
        safe_release(g_root_sig);
        safe_release(g_rtv_heap);
        g_width = 0;
        g_height = 0;
        g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;
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

        if (!create_input_texture_from_backbuffer(g_frames[0].backbuffer)) return false;
        if (!create_sharpen_pipeline()) return false;

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
        g_sharpen_enabled = !env_disabled(L"FSRINJ_DX12_SHARPEN");
        g_sharpness = env_float(L"FSRINJ_DX12_SHARPNESS", core::config().sharpness.load());
        if (g_sharpness <= 0.0f) g_sharpness = 0.20f;

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

        LOGF("[overlay-dx12] sharpen pass initialized on hwnd %p buffers=%u size=%ux%u format=%u queue=%p enabled=%s sharpness=%.2f",
             static_cast<void*>(g_hwnd), (unsigned)g_frames.size(), g_width, g_height, (unsigned)g_format,
             static_cast<void*>(g_queue), g_sharpen_enabled ? "on" : "off", g_sharpness);
        LOGF("[overlay-dx12] Dear ImGui is still bypassed on DX12; Home toggles the DX12 sharpen pass");
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

    bool render_sharpen(FrameContext& f) {
        if (!g_input || !g_srv_heap || !g_pso || !g_root_sig) return false;
        if (g_width == 0 || g_height == 0) return false;

        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(g_cmd, g_input, g_input_state, D3D12_RESOURCE_STATE_COPY_DEST);
        g_input_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_cmd->CopyResource(g_input, f.backbuffer);
        transition(g_cmd, g_input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_input_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_VIEWPORT vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(g_width);
        vp.Height = static_cast<float>(g_height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        D3D12_RECT scissor{ 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };

        g_cmd->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
        g_cmd->RSSetViewports(1, &vp);
        g_cmd->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[] = { g_srv_heap };
        g_cmd->SetDescriptorHeaps(1, heaps);
        g_cmd->SetGraphicsRootSignature(g_root_sig);
        g_cmd->SetPipelineState(g_pso);
        g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_cmd->SetGraphicsRootDescriptorTable(0, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
        const float params[4] = { 1.0f / static_cast<float>(g_width), 1.0f / static_cast<float>(g_height), g_sharpness, 0.0f };
        g_cmd->SetGraphicsRoot32BitConstants(1, 4, params, 0);
        g_cmd->DrawInstanced(3, 1, 0, 0);

        transition(g_cmd, f.backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return true;
    }
}

bool on_present(IDXGISwapChain* sc) {
    if (!g_init) {
        if (!init(sc)) return false;
        g_init = true;
        g_present_count = 0;
        LOGF("[overlay-dx12] init-only present skipped; sharpen begins after warmup");
        return true;
    }

    ++g_present_count;
    if (g_sharpen_enabled && g_present_count <= kWarmupPresents) {
        LOGF("[overlay-dx12] warmup present %u/%u; skipping sharpen", g_present_count, kWarmupPresents);
        return true;
    }

    static bool prev = false;
    bool down = (GetAsyncKeyState(core::config().toggle_key.load()) & 0x8000) != 0;
    if (down && !prev) {
        bool v = core::config().overlay_visible.load();
        core::config().overlay_visible.store(!v);
        LOGF("[overlay-dx12] Home toggle: DX12 sharpen %s", !v ? "visible/enabled" : "hidden/disabled");
    }
    prev = down;

    if (!g_sharpen_enabled) return true;
    if (!core::config().overlay_visible.load()) return true;

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

    if (!render_sharpen(f)) {
        LOGF("[overlay-dx12] render_sharpen returned false; skipping frame");
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
        LOGF("[overlay-dx12] first DX12 sharpen frame submitted successfully");
        g_logged_first_effect = true;
    }
    return true;
}

void on_resize_buffers() { release_frame_resources(); }
void on_after_resize(IDXGISwapChain* sc) {
    if (g_init && g_dev) {
        if (!create_render_targets(sc)) LOGF("[overlay-dx12] recreate sharpen resources failed after ResizeBuffers");
        else {
            g_present_count = 0;
            g_logged_first_effect = false;
            LOGF("[overlay-dx12] sharpen resources recreated after ResizeBuffers");
        }
    }
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
    g_sharpen_enabled = true;
    g_logged_first_effect = false;
    g_sharpness = 0.20f;
    g_present_count = 0;
    g_init = false;
}

} // namespace overlay::dx12
