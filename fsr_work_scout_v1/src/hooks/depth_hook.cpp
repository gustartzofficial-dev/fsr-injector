#include "hooks/depth_hook.h"
#include "core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>
#include <mutex>

#pragma comment(lib, "d3d11.lib")

namespace depth {
namespace {
    using OMSetRTFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                          ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    OMSetRTFn g_orig = nullptr;

    std::mutex            g_mtx;
    ID3D11Texture2D*      g_cand   = nullptr;   // current best depth texture (refs held)
    ID3D11ShaderResourceView* g_srv = nullptr;
    UINT                  g_w = 0, g_h = 0;
    DXGI_FORMAT           g_fmt = DXGI_FORMAT_UNKNOWN;
    bool                  g_readable = false;
    UINT                  g_best_area = 0;

    bool depthish(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_R32_TYPELESS:   case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R16_TYPELESS:   case DXGI_FORMAT_D16_UNORM:
                return true;
            default: return false;
        }
    }
    DXGI_FORMAT srv_format(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
                return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case DXGI_FORMAT_R32_TYPELESS:   case DXGI_FORMAT_D32_FLOAT:
                return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            case DXGI_FORMAT_R16_TYPELESS:   case DXGI_FORMAT_D16_UNORM:
                return DXGI_FORMAT_R16_UNORM;
            default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    void consider(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) {
        if (!depthish(d.Format)) return;
        if (!(d.BindFlags & D3D11_BIND_DEPTH_STENCIL)) return;
        float aspect = d.Height ? (float)d.Width / d.Height : 0.f;
        if (aspect < 1.2f || aspect > 2.4f) return;   // exclude square shadow maps
        UINT area = d.Width * d.Height;
        if (area <= g_best_area) return;               // keep the biggest screen-ish depth

        std::lock_guard<std::mutex> lk(g_mtx);
        if (area <= g_best_area) return;
        if (g_srv)  { g_srv->Release();  g_srv  = nullptr; }
        if (g_cand) { g_cand->Release(); g_cand = nullptr; }
        tex->AddRef();
        g_cand = tex; g_w = d.Width; g_h = d.Height; g_fmt = d.Format;
        g_best_area = area; g_readable = false;
    }

    void STDMETHODCALLTYPE hk_OMSetRT(ID3D11DeviceContext* ctx, UINT num,
                                      ID3D11RenderTargetView* const* rtvs,
                                      ID3D11DepthStencilView* dsv) {
        if (dsv) {
            ID3D11Resource* res = nullptr; dsv->GetResource(&res);
            if (res) {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
                    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
                    consider(tex, d);
                    tex->Release();
                }
                res->Release();
            }
        }
        g_orig(ctx, num, rtvs, dsv);
    }
}

bool install() {
    MH_Initialize();   // no-op/ignored if swapchain hooks already initialized it

    ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr) || !ctx) { LOGF("[depth] dummy device failed"); return false; }

    void** vt = *reinterpret_cast<void***>(ctx);
    void* target = vt[33];      // ID3D11DeviceContext::OMSetRenderTargets
    ctx->Release(); dev->Release();

    if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&hk_OMSetRT),
                      reinterpret_cast<void**>(&g_orig)) != MH_OK) { LOGF("[depth] create hook failed"); return false; }
    if (MH_EnableHook(target) != MH_OK) { LOGF("[depth] enable hook failed"); return false; }
    LOGF("[depth] OMSetRenderTargets hooked");
    return true;
}

ID3D11ShaderResourceView* current_srv() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_cand) return nullptr;
    if (g_srv) return g_srv;
    ID3D11Device* dev=nullptr; g_cand->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srv_format(g_fmt);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    HRESULT hr = dev->CreateShaderResourceView(g_cand, &sd, &g_srv);
    dev->Release();
    g_readable = SUCCEEDED(hr) && g_srv;
    if (!g_readable) { LOGF("[depth] %ux%u found but NOT readable (no SRV bind)", g_w, g_h); g_srv=nullptr; return nullptr; }
    LOGF("[depth] readable SRV created %ux%u", g_w, g_h);
    return g_srv;
}

bool found()    { return g_cand != nullptr; }
unsigned width(){ return g_w; }
unsigned height(){ return g_h; }
bool readable() { return g_readable; }
const char* fmt_name() {
    switch (g_fmt) {
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        case DXGI_FORMAT_R32_TYPELESS:   case DXGI_FORMAT_D32_FLOAT:         return "D32F";
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32FS8";
        case DXGI_FORMAT_R16_TYPELESS:   case DXGI_FORMAT_D16_UNORM:         return "D16";
        default: return "none";
    }
}
void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_srv)  { g_srv->Release();  g_srv=nullptr; }
    if (g_cand) { g_cand->Release(); g_cand=nullptr; }
}

} // namespace depth
