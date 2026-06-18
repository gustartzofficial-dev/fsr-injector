#include "fsr/upscaler.h"
#include "core/config.h"
#include "core/log.h"

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
    bool g_ready=false; UINT g_w=0,g_h=0; DXGI_FORMAT g_fmt=DXGI_FORMAT_UNKNOWN;

    struct CB { float invW,invH,strength,pad; };

    const char* kShader = R"(
        cbuffer CB:register(b0){ float invW,invH,strength,pad; };
        Texture2D tex:register(t0); SamplerState smp:register(s0);
        struct VSOut{ float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
        VSOut VSMain(uint id:SV_VertexID){ VSOut o; o.uv=float2((id<<1)&2,id&2);
            o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
        float luma(float3 c){ return dot(c,float3(0.299,0.587,0.114)); }

        // Contrast-adaptive sharpen with noise floor + deringing clamp.
        float4 PSMain(VSOut i):SV_Target{
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

    bool compile_one(const char* e,const char* t,ID3DBlob** o){
        ID3DBlob* err=nullptr;
        if(FAILED(D3DCompile(kShader,strlen(kShader),nullptr,nullptr,nullptr,e,t,0,0,o,&err))){
            LOGF("[up] compile %s failed: %s",e,err?(char*)err->GetBufferPointer():"?"); if(err)err->Release(); return false; }
        if(err)err->Release(); return true;
    }
    bool init(IDXGISwapChain* sc){
        if(g_ready) return true;
        if(FAILED(sc->GetDevice(__uuidof(ID3D11Device),(void**)&g_dev))) return false;
        g_dev->GetImmediateContext(&g_ctx);
        ID3DBlob *v=nullptr,*p=nullptr;
        if(!compile_one("VSMain","vs_5_0",&v)) return false;
        if(!compile_one("PSMain","ps_5_0",&p)){v->Release();return false;}
        g_dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&g_vs);
        g_dev->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&g_ps);
        v->Release();p->Release();
        D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; g_dev->CreateSamplerState(&sd,&g_smp);
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth=sizeof(CB); cbd.Usage=D3D11_USAGE_DEFAULT; cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; g_dev->CreateBuffer(&cbd,nullptr,&g_cb);
        g_ready=g_vs&&g_ps&&g_smp&&g_cb; if(g_ready) LOGF("[up] adaptive sharpener initialized");
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
    struct SB { ID3D11RenderTargetView* rtv=nullptr; ID3D11DepthStencilView* dsv=nullptr;
        D3D11_VIEWPORT vp[16]; UINT vpN=16; ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr;
        ID3D11InputLayout* il=nullptr; D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11ShaderResourceView* srv=nullptr;
        ID3D11SamplerState* smp=nullptr; ID3D11Buffer* cb=nullptr; };
    void save(SB& s){ g_ctx->OMGetRenderTargets(1,&s.rtv,&s.dsv); g_ctx->RSGetViewports(&s.vpN,s.vp);
        g_ctx->VSGetShader(&s.vs,nullptr,nullptr); g_ctx->PSGetShader(&s.ps,nullptr,nullptr); g_ctx->IAGetInputLayout(&s.il);
        g_ctx->IAGetPrimitiveTopology(&s.topo); g_ctx->PSGetShaderResources(0,1,&s.srv); g_ctx->PSGetSamplers(0,1,&s.smp); g_ctx->PSGetConstantBuffers(0,1,&s.cb); }
    void restore(SB& s){ g_ctx->OMSetRenderTargets(1,&s.rtv,s.dsv); if(s.vpN)g_ctx->RSSetViewports(s.vpN,s.vp);
        g_ctx->VSSetShader(s.vs,nullptr,0); g_ctx->PSSetShader(s.ps,nullptr,0); g_ctx->IASetInputLayout(s.il);
        g_ctx->IASetPrimitiveTopology(s.topo); g_ctx->PSSetShaderResources(0,1,&s.srv); g_ctx->PSSetSamplers(0,1,&s.smp); g_ctx->PSSetConstantBuffers(0,1,&s.cb);
        if(s.rtv)s.rtv->Release(); if(s.dsv)s.dsv->Release(); if(s.vs)s.vs->Release(); if(s.ps)s.ps->Release();
        if(s.il)s.il->Release(); if(s.srv)s.srv->Release(); if(s.smp)s.smp->Release(); if(s.cb)s.cb->Release(); }
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

    CB cb{ 1.f/g_w, 1.f/g_h, cfg.sharpness.load(), 0 };
    g_ctx->UpdateSubresource(g_cb,0,nullptr,&cb,0,0);

    SB s; save(s);
    ID3D11RenderTargetView* nrt=nullptr; g_ctx->OMSetRenderTargets(1,&nrt,nullptr);
    g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs,nullptr,0); g_ctx->PSSetShader(g_ps,nullptr,0);
    g_ctx->PSSetSamplers(0,1,&g_smp); g_ctx->PSSetConstantBuffers(0,1,&g_cb);
    g_ctx->PSSetShaderResources(0,1,&g_tmp_srv);
    D3D11_VIEWPORT vp{}; vp.Width=(float)g_w; vp.Height=(float)g_h; vp.MaxDepth=1.f; g_ctx->RSSetViewports(1,&vp);
    g_ctx->OMSetRenderTargets(1,&rtv,nullptr);
    g_ctx->Draw(3,0);
    ID3D11ShaderResourceView* nul=nullptr; g_ctx->PSSetShaderResources(0,1,&nul);
    restore(s);
    rtv->Release(); bb->Release();
}
void on_resize(){ if(g_tmp_srv){g_tmp_srv->Release();g_tmp_srv=nullptr;} if(g_tmp){g_tmp->Release();g_tmp=nullptr;} }
void shutdown(){ on_resize();
    if(g_cb)g_cb->Release(); if(g_smp)g_smp->Release(); if(g_ps)g_ps->Release(); if(g_vs)g_vs->Release();
    if(g_ctx)g_ctx->Release(); if(g_dev)g_dev->Release(); g_ready=false; }

} // namespace upscaler
