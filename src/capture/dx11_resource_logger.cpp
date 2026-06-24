#include "capture/dx11_resource_logger.h"
#include "core/log.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#pragma comment(lib, "d3d11.lib")

namespace capture::dx11log {
namespace {

using CreateTexture2DFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateSRVFn       = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using CreateRTVFn       = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**);
using CreateDSVFn       = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**);
using CreatePSFn        = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PSSetSRVFn        = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PSSetShaderFn     = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using DrawFn            = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn     = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DispatchFn        = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);

CreateTexture2DFn g_orig_create_tex2d = nullptr;
CreateSRVFn       g_orig_create_srv = nullptr;
CreateRTVFn       g_orig_create_rtv = nullptr;
CreateDSVFn       g_orig_create_dsv = nullptr;
CreatePSFn        g_orig_create_ps = nullptr;
PSSetSRVFn        g_orig_ps_set_srv = nullptr;
PSSetShaderFn     g_orig_ps_set_shader = nullptr;
DrawFn            g_orig_draw = nullptr;
DrawIndexedFn     g_orig_draw_indexed = nullptr;
DispatchFn        g_orig_dispatch = nullptr;

struct TexInfo {
    UINT width = 0;
    UINT height = 0;
    UINT mip_levels = 0;
    UINT array_size = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bind = 0;
    UINT misc = 0;
    UINT sample_count = 1;
    UINT create_index = 0;
    bool logged_candidate = false;
};

struct ViewInfo {
    ID3D11Resource* resource = nullptr; // weak key only
    TexInfo tex{};
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
};

std::mutex g_mtx;
std::unordered_map<ID3D11Resource*, TexInfo> g_textures;
std::unordered_map<ID3D11ShaderResourceView*, ViewInfo> g_srvs;
std::unordered_map<ID3D11RenderTargetView*, ViewInfo> g_rtvs;
std::unordered_map<ID3D11DepthStencilView*, ViewInfo> g_dsvs;
std::unordered_map<ID3D11PixelShader*, uint64_t> g_ps_hash;
std::array<ID3D11ShaderResourceView*, 128> g_last_ps_srvs{};
uint64_t g_current_ps_hash = 0;
UINT g_backbuffer_w = 0;
UINT g_backbuffer_h = 0;
DXGI_FORMAT g_backbuffer_fmt = DXGI_FORMAT_UNKNOWN;
uint64_t g_frame = 0;
uint64_t g_event = 0;
UINT g_create_index = 0;
UINT g_candidate_count = 0;
bool g_installed = false;

uint64_t fnv1a64(const void* data, size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

const char* fmt_name(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24";
        default: return "FMT_OTHER";
    }
}

std::string bind_flags(UINT b) {
    std::string s;
    if (b & D3D11_BIND_VERTEX_BUFFER) s += "VB|";
    if (b & D3D11_BIND_INDEX_BUFFER) s += "IB|";
    if (b & D3D11_BIND_CONSTANT_BUFFER) s += "CB|";
    if (b & D3D11_BIND_SHADER_RESOURCE) s += "SRV|";
    if (b & D3D11_BIND_STREAM_OUTPUT) s += "SO|";
    if (b & D3D11_BIND_RENDER_TARGET) s += "RTV|";
    if (b & D3D11_BIND_DEPTH_STENCIL) s += "DSV|";
    if (b & D3D11_BIND_UNORDERED_ACCESS) s += "UAV|";
    if (!s.empty()) s.pop_back();
    return s.empty() ? "none" : s;
}

bool is_depth_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

bool is_motion_format(DXGI_FORMAT f) {
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

bool is_color_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return true;
        default:
            return false;
    }
}

bool near_backbuffer(UINT w, UINT h) {
    if (!g_backbuffer_w || !g_backbuffer_h || !w || !h) return w >= 960 && h >= 540;
    return w >= g_backbuffer_w / 2 && h >= g_backbuffer_h / 2 && w <= g_backbuffer_w * 2 && h <= g_backbuffer_h * 2;
}

bool is_interesting_texture(const TexInfo& t) {
    if (!near_backbuffer(t.width, t.height)) return false;
    if ((t.bind & D3D11_BIND_DEPTH_STENCIL) || is_depth_format(t.format)) return true;
    if ((t.bind & D3D11_BIND_RENDER_TARGET) && is_color_format(t.format)) return true;
    if ((t.bind & D3D11_BIND_SHADER_RESOURCE) && is_motion_format(t.format)) return true;
    return false;
}

void log_texture_candidate(const char* tag, ID3D11Resource* res, const TexInfo& t, DXGI_FORMAT view_fmt = DXGI_FORMAT_UNKNOWN) {
    LOGF("[dx11log] %s tex#%u res=%p %ux%u fmt=%s view=%s bind=0x%X(%s) mips=%u array=%u samples=%u frame=%llu",
         tag, t.create_index, res, t.width, t.height, fmt_name(t.format), fmt_name(view_fmt), t.bind,
         bind_flags(t.bind).c_str(), t.mip_levels, t.array_size, t.sample_count,
         static_cast<unsigned long long>(g_frame));
}

TexInfo query_tex_info(ID3D11Resource* res) {
    TexInfo out{};
    if (!res) return out;
    auto it = g_textures.find(res);
    if (it != g_textures.end()) return it->second;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        out.width = d.Width;
        out.height = d.Height;
        out.mip_levels = d.MipLevels;
        out.array_size = d.ArraySize;
        out.format = d.Format;
        out.bind = d.BindFlags;
        out.misc = d.MiscFlags;
        out.sample_count = d.SampleDesc.Count;
        tex->Release();
    }
    return out;
}

HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT hr = g_orig_create_tex2d(dev, desc, init, out);
    if (SUCCEEDED(hr) && desc && out && *out) {
        std::lock_guard<std::mutex> lk(g_mtx);
        TexInfo t{};
        t.width = desc->Width;
        t.height = desc->Height;
        t.mip_levels = desc->MipLevels;
        t.array_size = desc->ArraySize;
        t.format = desc->Format;
        t.bind = desc->BindFlags;
        t.misc = desc->MiscFlags;
        t.sample_count = desc->SampleDesc.Count;
        t.create_index = ++g_create_index;
        ID3D11Resource* res = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&res))) && res) {
            g_textures[res] = t;
            res->Release();
        }
        if (is_interesting_texture(t)) {
            ++g_candidate_count;
            log_texture_candidate("CreateTexture2D", reinterpret_cast<ID3D11Resource*>(*out), t);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSRV(ID3D11Device* dev, ID3D11Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
    HRESULT hr = g_orig_create_srv(dev, res, desc, out);
    if (SUCCEEDED(hr) && res && out && *out) {
        std::lock_guard<std::mutex> lk(g_mtx);
        ViewInfo v{};
        v.resource = res;
        v.tex = query_tex_info(res);
        v.view_format = desc ? desc->Format : v.tex.format;
        g_srvs[*out] = v;
        if (is_interesting_texture(v.tex) || is_motion_format(v.view_format)) log_texture_candidate("CreateSRV", res, v.tex, v.view_format);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateRTV(ID3D11Device* dev, ID3D11Resource* res, const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** out) {
    HRESULT hr = g_orig_create_rtv(dev, res, desc, out);
    if (SUCCEEDED(hr) && res && out && *out) {
        std::lock_guard<std::mutex> lk(g_mtx);
        ViewInfo v{};
        v.resource = res;
        v.tex = query_tex_info(res);
        v.view_format = desc ? desc->Format : v.tex.format;
        g_rtvs[*out] = v;
        if (is_interesting_texture(v.tex)) log_texture_candidate("CreateRTV", res, v.tex, v.view_format);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateDSV(ID3D11Device* dev, ID3D11Resource* res, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, ID3D11DepthStencilView** out) {
    HRESULT hr = g_orig_create_dsv(dev, res, desc, out);
    if (SUCCEEDED(hr) && res && out && *out) {
        std::lock_guard<std::mutex> lk(g_mtx);
        ViewInfo v{};
        v.resource = res;
        v.tex = query_tex_info(res);
        v.view_format = desc ? desc->Format : v.tex.format;
        g_dsvs[*out] = v;
        log_texture_candidate("CreateDSV", res, v.tex, v.view_format);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(ID3D11Device* dev, const void* bytecode, SIZE_T len, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
    HRESULT hr = g_orig_create_ps(dev, bytecode, len, linkage, out);
    if (SUCCEEDED(hr) && bytecode && len && out && *out) {
        uint64_t h = fnv1a64(bytecode, len);
        std::lock_guard<std::mutex> lk(g_mtx);
        g_ps_hash[*out] = h;
        LOGF("[dx11log] CreatePixelShader ps=%p hash=%016llX bytes=%llu frame=%llu", *out,
             static_cast<unsigned long long>(h), static_cast<unsigned long long>(len),
             static_cast<unsigned long long>(g_frame));
    }
    return hr;
}

void STDMETHODCALLTYPE hk_PSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* classes, UINT class_count) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_current_ps_hash = 0;
        auto it = g_ps_hash.find(ps);
        if (it != g_ps_hash.end()) g_current_ps_hash = it->second;
        ++g_event;
    }
    g_orig_ps_set_shader(ctx, ps, classes, class_count);
}

void STDMETHODCALLTYPE hk_PSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11ShaderResourceView* const* srvs) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_event;
        for (UINT i = 0; i < count && (start + i) < g_last_ps_srvs.size(); ++i) {
            ID3D11ShaderResourceView* srv = srvs ? srvs[i] : nullptr;
            g_last_ps_srvs[start + i] = srv;
            auto it = g_srvs.find(srv);
            if (it == g_srvs.end()) continue;
            const ViewInfo& v = it->second;
            bool likely_motion = near_backbuffer(v.tex.width, v.tex.height) && (is_motion_format(v.tex.format) || is_motion_format(v.view_format));
            bool likely_depth = near_backbuffer(v.tex.width, v.tex.height) && (is_depth_format(v.tex.format) || is_depth_format(v.view_format));
            if (likely_motion || likely_depth) {
                LOGF("[dx11log] PSSetSRV frame=%llu event=%llu ps=%016llX slot=%u srv=%p res=%p %ux%u fmt=%s view=%s role=%s",
                     static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_event),
                     static_cast<unsigned long long>(g_current_ps_hash), start + i, srv, v.resource,
                     v.tex.width, v.tex.height, fmt_name(v.tex.format), fmt_name(v.view_format),
                     likely_motion ? "motion_like" : "depth_like");
            }
        }
    }
    g_orig_ps_set_srv(ctx, start, count, srvs);
}

void log_draw_if_interesting(const char* kind, UINT vertices_or_indices) {
    std::lock_guard<std::mutex> lk(g_mtx);
    ++g_event;
    bool has_motion = false;
    bool has_depth = false;
    for (UINT slot = 0; slot < g_last_ps_srvs.size(); ++slot) {
        auto it = g_srvs.find(g_last_ps_srvs[slot]);
        if (it == g_srvs.end()) continue;
        const ViewInfo& v = it->second;
        if (near_backbuffer(v.tex.width, v.tex.height) && (is_motion_format(v.tex.format) || is_motion_format(v.view_format))) has_motion = true;
        if (near_backbuffer(v.tex.width, v.tex.height) && (is_depth_format(v.tex.format) || is_depth_format(v.view_format))) has_depth = true;
    }
    if (has_motion || has_depth || vertices_or_indices <= 6) {
        LOGF("[dx11log] %s frame=%llu event=%llu ps=%016llX count=%u fullscreenish=%s hasMotionSRV=%s hasDepthSRV=%s",
             kind, static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_event),
             static_cast<unsigned long long>(g_current_ps_hash), vertices_or_indices,
             vertices_or_indices <= 6 ? "yes" : "no", has_motion ? "yes" : "no", has_depth ? "yes" : "no");
    }
}

void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex) {
    log_draw_if_interesting("Draw", vertex_count);
    g_orig_draw(ctx, vertex_count, start_vertex);
}

void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* ctx, UINT index_count, UINT start_index, INT base_vertex) {
    log_draw_if_interesting("DrawIndexed", index_count);
    g_orig_draw_indexed(ctx, index_count, start_index, base_vertex);
}

void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_event;
        LOGF("[dx11log] Dispatch frame=%llu event=%llu groups=%u,%u,%u", static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_event), x, y, z);
    }
    g_orig_dispatch(ctx, x, y, z);
}

bool hook(void* target, void* detour, void** orig, const char* name) {
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        LOGF("[dx11log] hook create failed: %s status=%d", name, static_cast<int>(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK && s != MH_ERROR_ENABLED) {
        LOGF("[dx11log] hook enable failed: %s status=%d", name, static_cast<int>(s));
        return false;
    }
    return true;
}

} // namespace

bool install() {
    if (g_installed) return true;
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LOGF("[dx11log] MH_Initialize failed status=%d", static_cast<int>(init));
        return false;
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr) || !dev || !ctx) {
        LOGF("[dx11log] dummy D3D11CreateDevice failed hr=0x%08lX", hr);
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        return false;
    }

    void** dvt = *reinterpret_cast<void***>(dev);
    void** cvt = *reinterpret_cast<void***>(ctx);

    bool ok = true;
    ok &= hook(dvt[5],  reinterpret_cast<void*>(&hk_CreateTexture2D), reinterpret_cast<void**>(&g_orig_create_tex2d), "CreateTexture2D");
    ok &= hook(dvt[7],  reinterpret_cast<void*>(&hk_CreateSRV),       reinterpret_cast<void**>(&g_orig_create_srv), "CreateShaderResourceView");
    ok &= hook(dvt[9],  reinterpret_cast<void*>(&hk_CreateRTV),       reinterpret_cast<void**>(&g_orig_create_rtv), "CreateRenderTargetView");
    ok &= hook(dvt[10], reinterpret_cast<void*>(&hk_CreateDSV),       reinterpret_cast<void**>(&g_orig_create_dsv), "CreateDepthStencilView");
    ok &= hook(dvt[15], reinterpret_cast<void*>(&hk_CreatePixelShader), reinterpret_cast<void**>(&g_orig_create_ps), "CreatePixelShader");
    ok &= hook(cvt[8],  reinterpret_cast<void*>(&hk_PSSetShaderResources), reinterpret_cast<void**>(&g_orig_ps_set_srv), "PSSetShaderResources");
    ok &= hook(cvt[9],  reinterpret_cast<void*>(&hk_PSSetShader), reinterpret_cast<void**>(&g_orig_ps_set_shader), "PSSetShader");
    ok &= hook(cvt[12], reinterpret_cast<void*>(&hk_DrawIndexed), reinterpret_cast<void**>(&g_orig_draw_indexed), "DrawIndexed");
    ok &= hook(cvt[13], reinterpret_cast<void*>(&hk_Draw), reinterpret_cast<void**>(&g_orig_draw), "Draw");
    ok &= hook(cvt[38], reinterpret_cast<void*>(&hk_Dispatch), reinterpret_cast<void**>(&g_orig_dispatch), "Dispatch");

    ctx->Release();
    dev->Release();

    g_installed = ok;
    LOGF("[dx11log] install %s", ok ? "ok" : "partial/failed");
    return ok;
}

void note_swapchain_present(IDXGISwapChain* sc) {
    if (!sc) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(sc->GetDesc(&desc))) {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_frame;
        if (desc.BufferDesc.Width && desc.BufferDesc.Height) {
            g_backbuffer_w = desc.BufferDesc.Width;
            g_backbuffer_h = desc.BufferDesc.Height;
            g_backbuffer_fmt = desc.BufferDesc.Format;
        }
        if ((g_frame % 120) == 1) {
            LOGF("[dx11log] Present frame=%llu backbuffer=%ux%u fmt=%s textures=%llu srvs=%llu rtvs=%llu dsvs=%llu candidates=%u",
                 static_cast<unsigned long long>(g_frame), g_backbuffer_w, g_backbuffer_h, fmt_name(g_backbuffer_fmt),
                 static_cast<unsigned long long>(g_textures.size()), static_cast<unsigned long long>(g_srvs.size()),
                 static_cast<unsigned long long>(g_rtvs.size()), static_cast<unsigned long long>(g_dsvs.size()), g_candidate_count);
        }
    }
}

void note_omset_render_targets(UINT num_rtvs, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    std::lock_guard<std::mutex> lk(g_mtx);
    ++g_event;
    if (dsv) {
        auto it = g_dsvs.find(dsv);
        if (it != g_dsvs.end()) {
            const ViewInfo& v = it->second;
            LOGF("[dx11log] OMSetRT frame=%llu event=%llu ps=%016llX dsv=%p res=%p %ux%u fmt=%s view=%s",
                 static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_event),
                 static_cast<unsigned long long>(g_current_ps_hash), dsv, v.resource, v.tex.width, v.tex.height,
                 fmt_name(v.tex.format), fmt_name(v.view_format));
        }
    }
    if (rtvs) {
        for (UINT i = 0; i < num_rtvs; ++i) {
            auto it = g_rtvs.find(rtvs[i]);
            if (it == g_rtvs.end()) continue;
            const ViewInfo& v = it->second;
            if (near_backbuffer(v.tex.width, v.tex.height)) {
                LOGF("[dx11log] OMSetRT frame=%llu event=%llu ps=%016llX rtvSlot=%u rtv=%p res=%p %ux%u fmt=%s view=%s",
                     static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_event),
                     static_cast<unsigned long long>(g_current_ps_hash), i, rtvs[i], v.resource, v.tex.width, v.tex.height,
                     fmt_name(v.tex.format), fmt_name(v.view_format));
            }
        }
    }
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_textures.clear();
    g_srvs.clear();
    g_rtvs.clear();
    g_dsvs.clear();
    g_ps_hash.clear();
    g_last_ps_srvs.fill(nullptr);
    g_installed = false;
}

} // namespace capture::dx11log
