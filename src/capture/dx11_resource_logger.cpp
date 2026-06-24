#include "capture/dx11_resource_logger.h"
#include "core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <mutex>
#include <unordered_set>
#include <cstdint>

#pragma comment(lib, "d3d11.lib")

namespace capture::dx11log {
namespace {
    using PSSetShaderResourcesFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
    using PSSetShaderFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
    using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
    using DrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using DrawInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
    using DrawIndexedInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

    PSSetShaderResourcesFn g_orig_ps_srvs = nullptr;
    PSSetShaderFn g_orig_ps_shader = nullptr;
    DrawFn g_orig_draw = nullptr;
    DrawIndexedFn g_orig_draw_indexed = nullptr;
    DrawInstancedFn g_orig_draw_instanced = nullptr;
    DrawIndexedInstancedFn g_orig_draw_indexed_instanced = nullptr;

    std::mutex g_mtx;
    ID3D11PixelShader* g_current_ps = nullptr;
    uint64_t g_draw_counter = 0;
    uint64_t g_srv_events = 0;
    uint64_t g_motion_candidates = 0;
    uint64_t g_depth_srv_candidates = 0;
    uint64_t g_color_candidates = 0;
    std::unordered_set<uint64_t> g_seen_srv_keys;
    std::unordered_set<uintptr_t> g_seen_ps;

    const char* fmt_name(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
            case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
            case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
            case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
            case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
            case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
            case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
            case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
            case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
            case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
            case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
            case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
            case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
            case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
            default: return "OTHER";
        }
    }

    bool screenish(UINT w, UINT h) {
        if (w < 640 || h < 360) return false;
        float aspect = h ? static_cast<float>(w) / static_cast<float>(h) : 0.0f;
        return aspect >= 1.2f && aspect <= 2.5f;
    }

    bool motion_like(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R32G32_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                return true;
            default:
                return false;
        }
    }

    bool depth_srv_like(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            case DXGI_FORMAT_R16_UNORM:
                return true;
            default:
                return false;
        }
    }

    bool color_like(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return true;
            default:
                return false;
        }
    }

    uint64_t srv_key(ID3D11Resource* res, DXGI_FORMAT fmt, UINT w, UINT h, UINT slot) {
        uintptr_t p = reinterpret_cast<uintptr_t>(res);
        return (static_cast<uint64_t>(p >> 4) ^ (static_cast<uint64_t>(fmt) << 48) ^
                (static_cast<uint64_t>(w) << 24) ^ static_cast<uint64_t>(h) ^
                (static_cast<uint64_t>(slot) << 56));
    }

    void log_srv_candidate(UINT slot, ID3D11ShaderResourceView* srv) {
        if (!srv) return;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        srv->GetDesc(&sd);

        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        if (!res) return;

        ID3D11Texture2D* tex = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) {
            res->Release();
            return;
        }

        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        const DXGI_FORMAT view_fmt = sd.Format != DXGI_FORMAT_UNKNOWN ? sd.Format : td.Format;
        const bool is_screenish = screenish(td.Width, td.Height);
        const bool is_motion = is_screenish && motion_like(view_fmt);
        const bool is_depth = is_screenish && depth_srv_like(view_fmt);
        const bool is_color = is_screenish && color_like(view_fmt);

        if (is_motion || is_depth || is_color) {
            std::lock_guard<std::mutex> lk(g_mtx);
            const uint64_t key = srv_key(res, view_fmt, td.Width, td.Height, slot);
            const bool first_time = g_seen_srv_keys.insert(key).second;
            if (is_motion) ++g_motion_candidates;
            if (is_depth) ++g_depth_srv_candidates;
            if (is_color) ++g_color_candidates;

            if (first_time || is_motion || (g_srv_events < 80)) {
                ++g_srv_events;
                const char* role = is_motion ? "motion-vector-candidate" : (is_depth ? "depth-srv-candidate" : "color/history-candidate");
                LOGF("[dx11-scout] %s slot=%u tex=%ux%u view=%s resource=%s bind=0x%X mips=%u samples=%u ps=0x%p draw=%llu seenMV=%llu seenDepthSRV=%llu seenColor=%llu",
                     role, slot, td.Width, td.Height, fmt_name(view_fmt), fmt_name(td.Format), td.BindFlags,
                     td.MipLevels, td.SampleDesc.Count, g_current_ps, static_cast<unsigned long long>(g_draw_counter),
                     static_cast<unsigned long long>(g_motion_candidates), static_cast<unsigned long long>(g_depth_srv_candidates),
                     static_cast<unsigned long long>(g_color_candidates));
            }
        }

        tex->Release();
        res->Release();
    }

    void STDMETHODCALLTYPE hk_PSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11ShaderResourceView* const* srvs) {
        if (srvs) {
            for (UINT i = 0; i < count; ++i) {
                log_srv_candidate(start + i, srvs[i]);
            }
        }
        g_orig_ps_srvs(ctx, start, count, srvs);
    }

    void STDMETHODCALLTYPE hk_PSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* shader, ID3D11ClassInstance* const* classes, UINT class_count) {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_current_ps = shader;
            uintptr_t p = reinterpret_cast<uintptr_t>(shader);
            if (shader && g_seen_ps.insert(p).second && g_seen_ps.size() <= 128) {
                LOGF("[dx11-scout] PSSetShader ps=0x%p draw=%llu uniquePS=%llu", shader,
                     static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_seen_ps.size()));
            }
        }
        g_orig_ps_shader(ctx, shader, classes, class_count);
    }

    void note_draw() {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_draw_counter;
        if (g_draw_counter == 1 || g_draw_counter == 100 || g_draw_counter == 1000 || (g_draw_counter % 5000) == 0) {
            LOGF("[dx11-scout] draw-progress draw=%llu mvCandidates=%llu depthSRV=%llu colorHistory=%llu uniquePS=%llu",
                 static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_motion_candidates),
                 static_cast<unsigned long long>(g_depth_srv_candidates), static_cast<unsigned long long>(g_color_candidates),
                 static_cast<unsigned long long>(g_seen_ps.size()));
        }
    }

    void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex) {
        note_draw();
        g_orig_draw(ctx, vertex_count, start_vertex);
    }

    void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* ctx, UINT index_count, UINT start_index, INT base_vertex) {
        note_draw();
        g_orig_draw_indexed(ctx, index_count, start_index, base_vertex);
    }

    void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext* ctx, UINT vc, UINT ic, UINT sv, UINT si) {
        note_draw();
        g_orig_draw_instanced(ctx, vc, ic, sv, si);
    }

    void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT icpi, UINT inst, UINT start, INT base, UINT start_inst) {
        note_draw();
        g_orig_draw_indexed_instanced(ctx, icpi, inst, start, base, start_inst);
    }
}

bool install() {
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LOGF("[dx11-scout] MH_Initialize failed mh=%d", (int)init);
        return false;
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr) || !ctx) {
        LOGF("[dx11-scout] dummy device failed hr=0x%08lX", hr);
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        return false;
    }

    void** vt = *reinterpret_cast<void***>(ctx);
    struct HookSpec { void* target; void* detour; void** orig; const char* name; };
    HookSpec hooks[] = {
        { vt[8],  reinterpret_cast<LPVOID>(&hk_PSSetShaderResources), reinterpret_cast<void**>(&g_orig_ps_srvs), "PSSetShaderResources" },
        { vt[9],  reinterpret_cast<LPVOID>(&hk_PSSetShader),          reinterpret_cast<void**>(&g_orig_ps_shader), "PSSetShader" },
        { vt[12], reinterpret_cast<LPVOID>(&hk_DrawIndexed),          reinterpret_cast<void**>(&g_orig_draw_indexed), "DrawIndexed" },
        { vt[13], reinterpret_cast<LPVOID>(&hk_Draw),                 reinterpret_cast<void**>(&g_orig_draw), "Draw" },
        { vt[20], reinterpret_cast<LPVOID>(&hk_DrawIndexedInstanced), reinterpret_cast<void**>(&g_orig_draw_indexed_instanced), "DrawIndexedInstanced" },
        { vt[21], reinterpret_cast<LPVOID>(&hk_DrawInstanced),        reinterpret_cast<void**>(&g_orig_draw_instanced), "DrawInstanced" },
    };

    bool ok = true;
    for (const HookSpec& h : hooks) {
        MH_STATUS cs = MH_CreateHook(h.target, h.detour, h.orig);
        if (cs != MH_OK && cs != MH_ERROR_ALREADY_CREATED) {
            LOGF("[dx11-scout] hook create failed %s mh=%d", h.name, (int)cs);
            ok = false;
            continue;
        }
        MH_STATUS es = MH_EnableHook(h.target);
        if (es != MH_OK && es != MH_ERROR_ENABLED) {
            LOGF("[dx11-scout] hook enable failed %s mh=%d", h.name, (int)es);
            ok = false;
        }
    }

    ctx->Release();
    dev->Release();

    if (!ok) return false;

    LOGF("[dx11-scout] installed after UI hook: PSSetShaderResources, PSSetShader, Draw/DrawIndexed/Instanced");
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_current_ps = nullptr;
    g_seen_srv_keys.clear();
    g_seen_ps.clear();
    g_draw_counter = 0;
    g_srv_events = 0;
    g_motion_candidates = 0;
    g_depth_srv_candidates = 0;
    g_color_candidates = 0;
}

} // namespace capture::dx11log
