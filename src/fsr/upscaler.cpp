#include "fsr/upscaler.h"
#include "core/config.h"
#include "core/log.h"
#include "fsr/fsr1_constants.h"
#include "fsr/ffx_include.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace upscaler {
namespace {
    ID3D11Device*        g_dev=nullptr;
    ID3D11DeviceContext* g_ctx=nullptr;
    ID3D11VertexShader*  g_vs=nullptr;
    ID3D11PixelShader*   g_ps=nullptr;
    ID3D11SamplerState*  g_smp=nullptr;
    ID3D11Buffer*        g_cb=nullptr;
    ID3D11Texture2D*     g_tmp=nullptr; ID3D11ShaderResourceView* g_tmp_srv=nullptr;
    // Explicit pipeline state for our fullscreen pass. Without these we inherit
    // whatever the game left bound at Present time -- an additive/alpha blend
    // state made the sharpened quad blend ONTO the frame instead of replacing
    // it, which showed up as the image randomly getting brighter.
    ID3D11BlendState*        g_blend=nullptr;   // opaque, write RGBA
    ID3D11DepthStencilState* g_dss=nullptr;     // depth test/write off
    ID3D11RasterizerState*   g_rs=nullptr;      // solid, no cull, no scissor
    bool g_ready=false; UINT g_w=0,g_h=0; DXGI_FORMAT g_fmt=DXGI_FORMAT_UNKNOWN;

    // Matches the HLSL cbuffer below. rcasCon holds AMD's packed RCAS constants;
    // fsrActive selects the real FidelityFX path at draw time.
    struct CB { float invW,invH,strength,fsrActive; unsigned rcasCon[4]; };
    bool g_fsr_real=false;
    fsr1::RcasConstants g_rcas_con{};
    float g_rcas_for_strength=-1.f;

    const char* kShader = R"(
        cbuffer CB:register(b0){ float invW,invH,strength,fsrActive; uint4 rcasCon; };
        Texture2D tex:register(t0); SamplerState smp:register(s0);
#ifdef FSRINJ_REAL_FSR
        // Genuine AMD FidelityFX RCAS (MIT). The DX11 path sharpens the finished
        // backbuffer at native resolution, which is exactly RCAS's design point.
        #define A_GPU 1
        #define A_HLSL 1
        #include "ffx_a.h"
        #define FSR_RCAS_F 1
        AF4 FsrRcasLoadF(ASU2 p){ return tex.Load(int3(p,0)); }
        void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b){}
        #include "ffx_fsr1.h"
#endif
        struct VSOut{ float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
        VSOut VSMain(uint id:SV_VertexID){ VSOut o; o.uv=float2((id<<1)&2,id&2);
            o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
        float luma(float3 c){ return dot(c,float3(0.299,0.587,0.114)); }

        // Contrast-adaptive sharpen with noise floor + deringing clamp.
        float4 PSMain(VSOut i):SV_Target{
#ifdef FSRINJ_REAL_FSR
            if(fsrActive>0.5){
                float3 rc; FsrRcasF(rc.r,rc.g,rc.b,AU2(i.pos.xy),rcasCon);
                return float4(saturate(rc),1);
            }
#endif
            float2 t=float2(invW,invH);
            float3 c=tex.SampleLevel(smp,i.uv,0).rgb;
            float3 n=tex.SampleLevel(smp,i.uv+float2(0,-1)*t,0).rgb;
            float3 s=tex.SampleLevel(smp,i.uv+float2(0, 1)*t,0).rgb;
            float3 e=tex.SampleLevel(smp,i.uv+float2( 1,0)*t,0).rgb;
            float3 w=tex.SampleLevel(smp,i.uv+float2(-1,0)*t,0).rgb;
            float3 mn=min(c,min(min(n,s),min(e,w)));
            float3 mx=max(c,max(max(n,s),max(e,w)));
            float contrast=luma(mx-mn);
            float noiseFloor=0.04;                       // below this = flat/noise -> don't sharpen
            float adapt=saturate((contrast-noiseFloor)/0.20);
            float amt=strength*adapt;
            float3 blur=(n+s+e+w)*0.25;
            float3 sharp=c+(c-blur)*amt;                 // unsharp mask
            sharp=clamp(sharp,mn,mx);                    // deringing: no overshoot beyond local range
            return float4(sharp,1);
        }
    )";

    bool compile_one(const char* e,const char* t,ID3DBlob** o,bool real_fsr,bool quiet=false){
        ID3DBlob* err=nullptr;
        const D3D_SHADER_MACRO defs[]={{"FSRINJ_REAL_FSR","1"},{nullptr,nullptr}};
        HRESULT hr=D3DCompile(kShader,strlen(kShader),"fsrinj_dx11",real_fsr?defs:nullptr,
                              &fsr1::include_handler(),e,t,0,0,o,&err);
        if(FAILED(hr)){
            LOGF("[up] compile %s failed%s: %s",e,quiet?" (falling back)":"",err?(char*)err->GetBufferPointer():"?");
            if(err)err->Release(); return false; }
        if(err)err->Release(); return true;
    }
    bool init(IDXGISwapChain* sc){
        if(g_ready) return true;
        if(FAILED(sc->GetDevice(__uuidof(ID3D11Device),(void**)&g_dev))) return false;
        g_dev->GetImmediateContext(&g_ctx);
        ID3DBlob *v=nullptr,*p=nullptr;
        // Try the real FidelityFX RCAS build first; fall back to the legacy
        // contrast-adaptive sharpener if it cannot compile (same policy as DX12).
        g_fsr_real = compile_one("VSMain","vs_5_0",&v,true,true) &&
                     compile_one("PSMain","ps_5_0",&p,true,true);
        if(!g_fsr_real){
            if(v){v->Release();v=nullptr;} if(p){p->Release();p=nullptr;}
            if(!compile_one("VSMain","vs_5_0",&v,false)) return false;
            if(!compile_one("PSMain","ps_5_0",&p,false)){v->Release();return false;}
        }
        g_dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&g_vs);
        g_dev->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&g_ps);
        v->Release();p->Release();
        D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; g_dev->CreateSamplerState(&sd,&g_smp);
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth=sizeof(CB); cbd.Usage=D3D11_USAGE_DEFAULT; cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; g_dev->CreateBuffer(&cbd,nullptr,&g_cb);
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable=FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd,&g_blend);
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable=FALSE; dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc=D3D11_COMPARISON_ALWAYS; dd.StencilEnable=FALSE;
        g_dev->CreateDepthStencilState(&dd,&g_dss);
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE;
        rd.DepthClipEnable=TRUE; rd.ScissorEnable=FALSE;
        g_dev->CreateRasterizerState(&rd,&g_rs);

        g_ready=g_vs&&g_ps&&g_smp&&g_cb&&g_blend&&g_dss&&g_rs;
        if(g_ready) LOGF("[up] DX11 sharpener initialized (%s)",
                         g_fsr_real?"AMD FidelityFX RCAS":"legacy contrast-adaptive");
        return g_ready;
    }
    bool ensure_tmp(ID3D11Texture2D* bb){
        D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d);
        if(g_tmp&&d.Width==g_w&&d.Height==g_h&&d.Format==g_fmt) return true;
        if(g_tmp_srv){g_tmp_srv->Release();g_tmp_srv=nullptr;} if(g_tmp){g_tmp->Release();g_tmp=nullptr;}
        g_w=d.Width;g_h=d.Height;g_fmt=d.Format;
        D3D11_TEXTURE2D_DESC td=d; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags=0;td.MiscFlags=0;td.SampleDesc.Count=1;
        if(FAILED(g_dev->CreateTexture2D(&td,nullptr,&g_tmp))) return false;
        return SUCCEEDED(g_dev->CreateShaderResourceView(g_tmp,nullptr,&g_tmp_srv));
    }
    // Full state block. UE4-era engines cache render state in their RHI and skip
    // redundant sets, so anything we change without restoring can corrupt later
    // frames. Blend/depth/raster/scissor are the ones that visibly matter.
    struct SB { ID3D11RenderTargetView* rtv=nullptr; ID3D11DepthStencilView* dsv=nullptr;
        D3D11_VIEWPORT vp[16]; UINT vpN=16; ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr;
        ID3D11InputLayout* il=nullptr; D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11ShaderResourceView* srv=nullptr;
        ID3D11SamplerState* smp=nullptr; ID3D11Buffer* cb=nullptr;
        ID3D11BlendState* bl=nullptr; FLOAT bf[4]{}; UINT mask=0xffffffff;
        ID3D11DepthStencilState* dss=nullptr; UINT sref=0;
        ID3D11RasterizerState* rs=nullptr;
        D3D11_RECT sc[16]{}; UINT scN=16; };
    void save(SB& s){ g_ctx->OMGetRenderTargets(1,&s.rtv,&s.dsv); g_ctx->RSGetViewports(&s.vpN,s.vp);
        g_ctx->VSGetShader(&s.vs,nullptr,nullptr); g_ctx->PSGetShader(&s.ps,nullptr,nullptr); g_ctx->IAGetInputLayout(&s.il);
        g_ctx->IAGetPrimitiveTopology(&s.topo); g_ctx->PSGetShaderResources(0,1,&s.srv); g_ctx->PSGetSamplers(0,1,&s.smp); g_ctx->PSGetConstantBuffers(0,1,&s.cb);
        g_ctx->OMGetBlendState(&s.bl,s.bf,&s.mask);
        g_ctx->OMGetDepthStencilState(&s.dss,&s.sref);
        g_ctx->RSGetState(&s.rs);
        g_ctx->RSGetScissorRects(&s.scN,s.sc); }
    void restore(SB& s){ g_ctx->OMSetRenderTargets(1,&s.rtv,s.dsv); if(s.vpN)g_ctx->RSSetViewports(s.vpN,s.vp);
        g_ctx->VSSetShader(s.vs,nullptr,0); g_ctx->PSSetShader(s.ps,nullptr,0); g_ctx->IASetInputLayout(s.il);
        g_ctx->IASetPrimitiveTopology(s.topo); g_ctx->PSSetShaderResources(0,1,&s.srv); g_ctx->PSSetSamplers(0,1,&s.smp); g_ctx->PSSetConstantBuffers(0,1,&s.cb);
        g_ctx->OMSetBlendState(s.bl,s.bf,s.mask);
        g_ctx->OMSetDepthStencilState(s.dss,s.sref);
        g_ctx->RSSetState(s.rs);
        if(s.scN)g_ctx->RSSetScissorRects(s.scN,s.sc);
        if(s.rtv)s.rtv->Release(); if(s.dsv)s.dsv->Release(); if(s.vs)s.vs->Release(); if(s.ps)s.ps->Release();
        if(s.il)s.il->Release(); if(s.srv)s.srv->Release(); if(s.smp)s.smp->Release(); if(s.cb)s.cb->Release();
        if(s.bl)s.bl->Release(); if(s.dss)s.dss->Release(); if(s.rs)s.rs->Release(); }
}

void sharpen(IDXGISwapChain* sc){
    auto& cfg=core::config();
    if(!cfg.upscaling_enabled.load()) return;
    if(!init(sc)) return;
    ID3D11Texture2D* bb=nullptr;
    if(FAILED(sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb))||!bb) return;
    if(!ensure_tmp(bb)){ bb->Release(); return; }

    g_ctx->CopyResource(g_tmp,bb);                       // source = current frame
    ID3D11RenderTargetView* rtv=nullptr;
    if(FAILED(g_dev->CreateRenderTargetView(bb,nullptr,&rtv))||!rtv){ bb->Release(); return; }

    const float strength=cfg.sharpness.load();
    if(g_fsr_real && g_rcas_for_strength!=strength){
        fsr1::rcas_constants(g_rcas_con, fsr1::slider_to_stops(strength));
        g_rcas_for_strength=strength;
    }
    CB cb{ 1.f/g_w, 1.f/g_h, strength, g_fsr_real?1.f:0.f };
    cb.rcasCon[0]=g_rcas_con.con[0]; cb.rcasCon[1]=g_rcas_con.con[1];
    cb.rcasCon[2]=g_rcas_con.con[2]; cb.rcasCon[3]=g_rcas_con.con[3];
    g_ctx->UpdateSubresource(g_cb,0,nullptr,&cb,0,0);

    SB s; save(s);
    ID3D11RenderTargetView* nrt=nullptr; g_ctx->OMSetRenderTargets(1,&nrt,nullptr);
    g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs,nullptr,0); g_ctx->PSSetShader(g_ps,nullptr,0);
    g_ctx->PSSetSamplers(0,1,&g_smp); g_ctx->PSSetConstantBuffers(0,1,&g_cb);
    g_ctx->PSSetShaderResources(0,1,&g_tmp_srv);
    D3D11_VIEWPORT vp{}; vp.Width=(float)g_w; vp.Height=(float)g_h; vp.MaxDepth=1.f; g_ctx->RSSetViewports(1,&vp);
    // Force opaque write, no depth, no scissor -- never inherit the game's.
    const FLOAT bf[4]={0,0,0,0};
    g_ctx->OMSetBlendState(g_blend,bf,0xffffffff);
    g_ctx->OMSetDepthStencilState(g_dss,0);
    g_ctx->RSSetState(g_rs);
    g_ctx->OMSetRenderTargets(1,&rtv,nullptr);
    g_ctx->Draw(3,0);
    ID3D11ShaderResourceView* nul=nullptr; g_ctx->PSSetShaderResources(0,1,&nul);
    restore(s);
    rtv->Release(); bb->Release();
}
void on_resize(){ if(g_tmp_srv){g_tmp_srv->Release();g_tmp_srv=nullptr;} if(g_tmp){g_tmp->Release();g_tmp=nullptr;} }
void shutdown(){ on_resize();
    if(g_rs)g_rs->Release(); if(g_dss)g_dss->Release(); if(g_blend)g_blend->Release();
    g_rs=nullptr; g_dss=nullptr; g_blend=nullptr;
    if(g_cb)g_cb->Release(); if(g_smp)g_smp->Release(); if(g_ps)g_ps->Release(); if(g_vs)g_vs->Release();
    if(g_ctx)g_ctx->Release(); if(g_dev)g_dev->Release(); g_ready=false; }

} // namespace upscaler
