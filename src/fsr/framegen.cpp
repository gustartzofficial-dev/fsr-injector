#include "fsr/framegen.h"
#include "core/config.h"
#include "core/log.h"
#include "hooks/depth_hook.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

namespace framegen {
namespace {

    std::atomic<uint64_t> g_real{0};
    std::atomic<uint64_t> g_gen{0};

    ID3D11Device*        g_dev = nullptr;
    ID3D11DeviceContext* g_ctx = nullptr;

    ID3D11VertexShader*  g_vs        = nullptr;
    ID3D11PixelShader*   g_ps_flow   = nullptr;   // block-matching optical flow
    ID3D11PixelShader*   g_ps_smooth = nullptr;   // flow smoothing (outlier removal)
    ID3D11PixelShader*   g_ps_interp = nullptr;   // motion-compensated interpolation
    ID3D11SamplerState*  g_smp       = nullptr;
    ID3D11BlendState*    g_blend     = nullptr;
    ID3D11Buffer*        g_cb        = nullptr;

    ID3D11Texture2D* g_prev_tex=nullptr; ID3D11ShaderResourceView* g_prev_srv=nullptr;
    ID3D11Texture2D* g_curr_tex=nullptr; ID3D11ShaderResourceView* g_curr_srv=nullptr;
    // two low-res RGBA16F flow buffers (xy = px offset, z = confidence): raw + smoothed
    ID3D11Texture2D* g_flow1=nullptr; ID3D11RenderTargetView* g_flow1_rtv=nullptr; ID3D11ShaderResourceView* g_flow1_srv=nullptr;
    ID3D11Texture2D* g_flow2=nullptr; ID3D11RenderTargetView* g_flow2_rtv=nullptr; ID3D11ShaderResourceView* g_flow2_srv=nullptr;

    bool g_ready=false, g_have_prev=false;
    UINT g_w=0,g_h=0,g_lw=0,g_lh=0;
    DXGI_FORMAT g_fmt=DXGI_FORMAT_UNKNOWN;

    const int kDS=8, kSearchR=12, kSearchS=2, kPatchP=1;

    struct FlowCB { unsigned W,H,lowW,lowH; float invW,invH; int searchR,searchS; int patchP,ds,useDepth,pad; };

    const char* kShader = R"(
        cbuffer FlowCB : register(b0) {
            uint W,H,lowW,lowH; float invW,invH; int searchR,searchS; int patchP,ds,useDepth,pad;
        };
        Texture2D texPrev:register(t0); Texture2D texCurr:register(t1); Texture2D flowTex:register(t2);
        Texture2D depthTex:register(t3);
        SamplerState smp:register(s0);
        struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
        VSOut VSMain(uint id:SV_VertexID){ VSOut o; o.uv=float2((id<<1)&2,id&2);
            o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
        float luma(float3 c){ return dot(c,float3(0.299,0.587,0.114)); }

        // One SAD evaluation: compares a 3x3 patch of curr vs prev shifted by candPx.
        // 'spreadPx' widens the patch taps to emulate a coarser pyramid level.
        float sad_at(float2 cuv, float2 candPx, float spreadPx){
            float2 offUv = candPx*float2(invW,invH);
            float sad=0;
            [unroll] for(int py=-1;py<=1;py++)
            [unroll] for(int px=-1;px<=1;px++){
                float2 p=cuv+float2(px,py)*float2(invW,invH)*spreadPx;
                sad+=abs(luma(texCurr.SampleLevel(smp,p,0).rgb)-luma(texPrev.SampleLevel(smp,p+offUv,0).rgb));
            }
            return sad;
        }

        // Coarse-to-fine ("pyramid") optical flow: a wide cheap search first, then two
        // refinement passes. Reaches ~+-20px while staying cheaper than the old fixed
        // +-12px single search, and tracks fast motion far better.
        float4 PSFlow(VSOut i):SV_Target{
            float2 cuv=i.uv; float2 flowPx=float2(0,0); float bestSad=1e20;
            [unroll] for(int lvl=0; lvl<3; lvl++){
                float stepPx  = (lvl==0)?8.0      :((lvl==1)?3.0 :1.0);
                float spreadPx= (lvl==0)?(ds*2.0) :((lvl==1)?(float)ds:(ds*0.5));
                int   rng     = (lvl==0)?2:1;
                float bSad=1e20; float2 bO=flowPx;
                [loop] for(int oy=-rng;oy<=rng;oy++)
                [loop] for(int ox=-rng;ox<=rng;ox++){
                    float2 candPx=flowPx+float2(ox,oy)*stepPx;
                    float sad=sad_at(cuv,candPx,spreadPx);
                    if(sad<bSad){bSad=sad;bO=candPx;}
                }
                flowPx=bO; bestSad=bSad;
            }
            float conf=saturate(1.0 - bestSad/1.5);   // low residual -> trust this vector
            return float4(flowPx,conf,1);
        }

        // 3x3 average of the flow field: kills speckle / outlier vectors.
        float4 PSFlowSmooth(VSOut i):SV_Target{
            float2 ts=float2(1.0/lowW,1.0/lowH); float4 acc=0;
            [unroll] for(int y=-1;y<=1;y++) [unroll] for(int x=-1;x<=1;x++)
                acc+=flowTex.SampleLevel(smp,i.uv+float2(x,y)*ts,0);
            return acc/9.0;
        }

        // Interp at t=0.5; fall back to plain blend where flow is unreliable.
        float4 PSMain(VSOut i):SV_Target{
            float4 f=flowTex.SampleLevel(smp,i.uv,0);
            float2 ouv=f.xy*float2(invW,invH); float conf=saturate(f.z);
            float4 a=texPrev.SampleLevel(smp,i.uv+0.5*ouv,0);
            float4 b=texCurr.SampleLevel(smp,i.uv-0.5*ouv,0);
            float consist=saturate(1.0-abs(luma(a.rgb)-luma(b.rgb))*4.0);
            float w=conf*consist;
            if(useDepth==1){
                float dc=depthTex.SampleLevel(smp,i.uv,0).r;
                float dx=abs(depthTex.SampleLevel(smp,i.uv+float2(invW,0),0).r-dc);
                float dy=abs(depthTex.SampleLevel(smp,i.uv+float2(0,invH),0).r-dc);
                float edge=saturate((dx+dy)*40.0);   // depth silhouette = disocclusion risk
                w*=(1.0-edge);                        // distrust warp on silhouettes
            }
            float4 pc=texPrev.SampleLevel(smp,i.uv,0);
            float4 cc=texCurr.SampleLevel(smp,i.uv,0);
            float4 plain=lerp(pc,cc,0.5);
            float4 warped=lerp(a,b,0.5);
            float4 outc=lerp(plain,warped,w);
            // HUD/UI protection: where the two real frames are ~identical (a static
            // overlay), bypass warping entirely so HUD text / crosshairs do not smear.
            float3 d3=abs(pc.rgb-cc.rgb); float chg=max(d3.r,max(d3.g,d3.b));
            float staticMask=saturate(1.0 - chg*50.0);
            outc.rgb=lerp(outc.rgb,cc.rgb,staticMask);
            return outc;
        }
    )";

    bool compile_one(const char* e,const char* t,ID3DBlob** o){
        ID3DBlob* err=nullptr;
        if(FAILED(D3DCompile(kShader,strlen(kShader),nullptr,nullptr,nullptr,e,t,0,0,o,&err))){
            LOGF("[fg] compile %s failed: %s",e,err?(char*)err->GetBufferPointer():"?");
            if(err)err->Release(); return false; }
        if(err)err->Release(); return true;
    }
    bool compile_pipeline(){
        ID3DBlob *v=nullptr,*pf=nullptr,*psm=nullptr,*pi=nullptr;
        if(!compile_one("VSMain","vs_5_0",&v)) return false;
        if(!compile_one("PSFlow","ps_5_0",&pf)){v->Release();return false;}
        if(!compile_one("PSFlowSmooth","ps_5_0",&psm)){v->Release();pf->Release();return false;}
        if(!compile_one("PSMain","ps_5_0",&pi)){v->Release();pf->Release();psm->Release();return false;}
        g_dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&g_vs);
        g_dev->CreatePixelShader(pf->GetBufferPointer(),pf->GetBufferSize(),nullptr,&g_ps_flow);
        g_dev->CreatePixelShader(psm->GetBufferPointer(),psm->GetBufferSize(),nullptr,&g_ps_smooth);
        g_dev->CreatePixelShader(pi->GetBufferPointer(),pi->GetBufferSize(),nullptr,&g_ps_interp);
        v->Release();pf->Release();psm->Release();pi->Release();
        D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; g_dev->CreateSamplerState(&sd,&g_smp);
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL; g_dev->CreateBlendState(&bd,&g_blend);
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth=sizeof(FlowCB); cbd.Usage=D3D11_USAGE_DEFAULT; cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; g_dev->CreateBuffer(&cbd,nullptr,&g_cb);
        return g_vs&&g_ps_flow&&g_ps_smooth&&g_ps_interp&&g_smp&&g_blend&&g_cb;
    }

    void rel(){
        if(g_prev_srv){g_prev_srv->Release();g_prev_srv=nullptr;} if(g_prev_tex){g_prev_tex->Release();g_prev_tex=nullptr;}
        if(g_curr_srv){g_curr_srv->Release();g_curr_srv=nullptr;} if(g_curr_tex){g_curr_tex->Release();g_curr_tex=nullptr;}
        if(g_flow1_srv){g_flow1_srv->Release();g_flow1_srv=nullptr;} if(g_flow1_rtv){g_flow1_rtv->Release();g_flow1_rtv=nullptr;} if(g_flow1){g_flow1->Release();g_flow1=nullptr;}
        if(g_flow2_srv){g_flow2_srv->Release();g_flow2_srv=nullptr;} if(g_flow2_rtv){g_flow2_rtv->Release();g_flow2_rtv=nullptr;} if(g_flow2){g_flow2->Release();g_flow2=nullptr;}
        g_have_prev=false;
    }
    bool mk_cap(const D3D11_TEXTURE2D_DESC& bb,ID3D11Texture2D** t,ID3D11ShaderResourceView** s){
        D3D11_TEXTURE2D_DESC d=bb; d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags=0;d.MiscFlags=0;d.SampleDesc.Count=1;d.SampleDesc.Quality=0;
        if(FAILED(g_dev->CreateTexture2D(&d,nullptr,t)))return false;
        return SUCCEEDED(g_dev->CreateShaderResourceView(*t,nullptr,s));
    }
    bool mk_flow(ID3D11Texture2D** t,ID3D11RenderTargetView** r,ID3D11ShaderResourceView** s){
        D3D11_TEXTURE2D_DESC d{}; d.Width=g_lw;d.Height=g_lh;d.MipLevels=1;d.ArraySize=1;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(g_dev->CreateTexture2D(&d,nullptr,t)))return false;
        if(FAILED(g_dev->CreateRenderTargetView(*t,nullptr,r)))return false;
        return SUCCEEDED(g_dev->CreateShaderResourceView(*t,nullptr,s));
    }
    bool ensure(ID3D11Texture2D* bb){
        D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d);
        if(g_prev_tex&&d.Width==g_w&&d.Height==g_h&&d.Format==g_fmt) return true;
        rel(); g_w=d.Width;g_h=d.Height;g_fmt=d.Format;
        g_lw=(std::max)(1u,(g_w+kDS-1)/kDS); g_lh=(std::max)(1u,(g_h+kDS-1)/kDS);
        if(!mk_cap(d,&g_prev_tex,&g_prev_srv))return false;
        if(!mk_cap(d,&g_curr_tex,&g_curr_srv))return false;
        if(!mk_flow(&g_flow1,&g_flow1_rtv,&g_flow1_srv))return false;
        if(!mk_flow(&g_flow2,&g_flow2_rtv,&g_flow2_srv))return false;
        FlowCB cb{g_w,g_h,g_lw,g_lh,1.f/g_w,1.f/g_h,kSearchR,kSearchS,kPatchP,kDS,0,0};
        g_ctx->UpdateSubresource(g_cb,0,nullptr,&cb,0,0);
        LOGF("[fg] resources %ux%u flow=%ux%u (smooth+confidence)",g_w,g_h,g_lw,g_lh);
        return true;
    }
    bool lazy(IDXGISwapChain* sc){
        if(g_ready)return true;
        if(FAILED(sc->GetDevice(__uuidof(ID3D11Device),(void**)&g_dev)))return false;
        g_dev->GetImmediateContext(&g_ctx);
        if(!compile_pipeline()){LOGF("[fg] pipeline failed");return false;}
        g_ready=true; LOGF("[fg] initialized (flow + smoothing + confidence)"); return true;
    }

    struct SB { ID3D11RenderTargetView* rtv=nullptr; ID3D11DepthStencilView* dsv=nullptr;
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT vpN=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr; ID3D11InputLayout* il=nullptr; D3D11_PRIMITIVE_TOPOLOGY topo;
        ID3D11ShaderResourceView* srv[4]={}; ID3D11SamplerState* smp=nullptr; ID3D11BlendState* bl=nullptr; FLOAT bf[4]; UINT mk=0; ID3D11Buffer* cb=nullptr; };
    void save(SB& s){ g_ctx->OMGetRenderTargets(1,&s.rtv,&s.dsv); g_ctx->RSGetViewports(&s.vpN,s.vp);
        g_ctx->VSGetShader(&s.vs,nullptr,nullptr); g_ctx->PSGetShader(&s.ps,nullptr,nullptr); g_ctx->IAGetInputLayout(&s.il);
        g_ctx->IAGetPrimitiveTopology(&s.topo); g_ctx->PSGetShaderResources(0,4,s.srv); g_ctx->PSGetSamplers(0,1,&s.smp);
        g_ctx->OMGetBlendState(&s.bl,s.bf,&s.mk); g_ctx->PSGetConstantBuffers(0,1,&s.cb); }
    void restore(SB& s){ g_ctx->OMSetRenderTargets(1,&s.rtv,s.dsv); if(s.vpN)g_ctx->RSSetViewports(s.vpN,s.vp);
        g_ctx->VSSetShader(s.vs,nullptr,0); g_ctx->PSSetShader(s.ps,nullptr,0); g_ctx->IASetInputLayout(s.il);
        g_ctx->IASetPrimitiveTopology(s.topo); g_ctx->PSSetShaderResources(0,4,s.srv); g_ctx->PSSetSamplers(0,1,&s.smp);
        g_ctx->OMSetBlendState(s.bl,s.bf,s.mk); g_ctx->PSSetConstantBuffers(0,1,&s.cb);
        if(s.rtv)s.rtv->Release(); if(s.dsv)s.dsv->Release(); if(s.vs)s.vs->Release(); if(s.ps)s.ps->Release();
        if(s.il)s.il->Release(); for(int i=0;i<4;i++) if(s.srv[i])s.srv[i]->Release(); if(s.smp)s.smp->Release(); if(s.bl)s.bl->Release(); if(s.cb)s.cb->Release(); }

    void pass(ID3D11RenderTargetView* rt,float vw,float vh,ID3D11PixelShader* ps,
              ID3D11ShaderResourceView* a,ID3D11ShaderResourceView* b,ID3D11ShaderResourceView* c){
        ID3D11RenderTargetView* nrt=nullptr; g_ctx->OMSetRenderTargets(1,&nrt,nullptr); // detach first
        ID3D11ShaderResourceView* srv[3]={a,b,c}; g_ctx->PSSetShaderResources(0,3,srv);
        g_ctx->OMSetRenderTargets(1,&rt,nullptr);
        D3D11_VIEWPORT vp{}; vp.Width=vw; vp.Height=vh; vp.MaxDepth=1.f; g_ctx->RSSetViewports(1,&vp);
        g_ctx->PSSetShader(ps,nullptr,0); g_ctx->Draw(3,0);
    }

    void draw_interpolated(ID3D11Texture2D* backbuffer){
        ID3D11RenderTargetView* bbrtv=nullptr;
        if(FAILED(g_dev->CreateRenderTargetView(backbuffer,nullptr,&bbrtv))||!bbrtv) return;
        SB s; save(s);
        const FLOAT bf[4]={0,0,0,0}; g_ctx->OMSetBlendState(g_blend,bf,0xffffffff);
        g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs,nullptr,0); g_ctx->PSSetSamplers(0,1,&g_smp); g_ctx->PSSetConstantBuffers(0,1,&g_cb);
        ID3D11ShaderResourceView* nul=nullptr;
        pass(g_flow1_rtv,(float)g_lw,(float)g_lh,g_ps_flow,  g_prev_srv,g_curr_srv,nul);       // raw flow
        pass(g_flow2_rtv,(float)g_lw,(float)g_lh,g_ps_smooth,nul,nul,g_flow1_srv);             // smoothed flow

        // Optional depth-assisted disocclusion (default off; only if a readable depth exists).
        ID3D11ShaderResourceView* dsrv = core::config().use_depth.load() ? depth::current_srv() : nullptr;
        FlowCB cb{g_w,g_h,g_lw,g_lh,1.f/g_w,1.f/g_h,kSearchR,kSearchS,kPatchP,kDS, dsrv?1:0, 0};
        g_ctx->UpdateSubresource(g_cb,0,nullptr,&cb,0,0);
        g_ctx->PSSetShaderResources(3,1,&dsrv);

        pass(bbrtv,(float)g_w,(float)g_h,g_ps_interp, g_prev_srv,g_curr_srv,g_flow2_srv);      // interpolate
        ID3D11ShaderResourceView* nul4[4]={nullptr,nullptr,nullptr,nullptr}; g_ctx->PSSetShaderResources(0,4,nul4);
        restore(s); bbrtv->Release();
    }
}

void before_present(IDXGISwapChain* sc, PresentTrampoline present, unsigned flags){
    g_real.fetch_add(1,std::memory_order_relaxed);
    if(!core::config().framegen_enabled.load()){ g_have_prev=false; return; }
    if(!lazy(sc)) return;
    ID3D11Texture2D* bb=nullptr;
    if(FAILED(sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb))||!bb) return;
    if(!ensure(bb)){ bb->Release(); return; }
    g_ctx->CopyResource(g_curr_tex,bb);
    if(g_have_prev){
        draw_interpolated(bb);
        if (core::config().dx11_frame_pacing.load()) {
            present(sc,0,flags);
            g_gen.fetch_add(1,std::memory_order_relaxed);
        }
        g_ctx->CopyResource(bb,g_curr_tex);
    }
    std::swap(g_prev_tex,g_curr_tex); std::swap(g_prev_srv,g_curr_srv);
    g_have_prev=true; bb->Release();
}
void on_resize(){ rel(); }
void shutdown(){ rel();
    if(g_cb)g_cb->Release(); if(g_blend)g_blend->Release(); if(g_smp)g_smp->Release();
    if(g_ps_interp)g_ps_interp->Release(); if(g_ps_smooth)g_ps_smooth->Release(); if(g_ps_flow)g_ps_flow->Release(); if(g_vs)g_vs->Release();
    if(g_ctx)g_ctx->Release(); if(g_dev)g_dev->Release(); g_ready=false; }
uint64_t real_frames(){ return g_real.load(std::memory_order_relaxed); }
uint64_t generated_frames(){ return g_gen.load(std::memory_order_relaxed); }

} // namespace framegen
