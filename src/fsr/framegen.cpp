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
    ID3D11Texture2D* g_gen_tex=nullptr; ID3D11RenderTargetView* g_gen_rtv=nullptr;
    bool g_generated_ready=false;

    LARGE_INTEGER g_qpc_freq{};
    LARGE_INTEGER g_last_real_present{};
    double g_real_interval_sec=0.0;
    bool g_have_real_timing=false;
    bool g_inside_generated_present=false;
    bool g_pacing_enabled=true;
    float g_pace_fraction=0.50f;

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

        float patch_error_lvl(float2 currUv, float2 prevUv, float2 spread){
            float e=0.0;
            e += abs(luma(texCurr.SampleLevel(smp,currUv,0).rgb)-luma(texPrev.SampleLevel(smp,prevUv,0).rgb))*2.0;
            e += abs(luma(texCurr.SampleLevel(smp,currUv+float2( spread.x,0),0).rgb)-luma(texPrev.SampleLevel(smp,prevUv+float2( spread.x,0),0).rgb));
            e += abs(luma(texCurr.SampleLevel(smp,currUv+float2(-spread.x,0),0).rgb)-luma(texPrev.SampleLevel(smp,prevUv+float2(-spread.x,0),0).rgb));
            e += abs(luma(texCurr.SampleLevel(smp,currUv+float2(0, spread.y),0).rgb)-luma(texPrev.SampleLevel(smp,prevUv+float2(0, spread.y),0).rgb));
            e += abs(luma(texCurr.SampleLevel(smp,currUv+float2(0,-spread.y),0).rgb)-luma(texPrev.SampleLevel(smp,prevUv+float2(0,-spread.y),0).rgb));
            return e;
        }

        // Coarse-to-fine pyramid-style optical flow. This mirrors the DX12 path:
        // first search wide/blurry motion, then refine around that vector. It is
        // still final-frame optical flow, not native engine motion vectors.
        float4 PSFlow(VSOut i):SV_Target{
            float2 cuv=i.uv;
            float2 px=float2(invW,invH);
            float2 flow=float2(0,0);
            float best=1e20, second=1e20;

            // Level 0: wide search, coarse details.
            {
                float2 stepPx=px*7.0;
                float2 spread=px*5.0;
                float2 bestFlow=flow; best=1e20; second=1e20;
                [unroll] for(int y=-2;y<=2;y++) [unroll] for(int x=-2;x<=2;x++){
                    float2 cand=flow+float2((float)x,(float)y)*stepPx;
                    float err=patch_error_lvl(cuv,cuv+cand,spread);
                    if(err<best){ second=best; best=err; bestFlow=cand; }
                    else if(err<second) second=err;
                }
                flow=bestFlow;
            }
            // Level 1: medium refine.
            {
                float2 stepPx=px*3.0;
                float2 spread=px*2.0;
                float2 bestFlow=flow; best=1e20; second=1e20;
                [unroll] for(int y=-1;y<=1;y++) [unroll] for(int x=-1;x<=1;x++){
                    float2 cand=flow+float2((float)x,(float)y)*stepPx;
                    float err=patch_error_lvl(cuv,cuv+cand,spread);
                    if(err<best){ second=best; best=err; bestFlow=cand; }
                    else if(err<second) second=err;
                }
                flow=bestFlow;
            }
            // Level 2: fine single-pixel refine.
            {
                float2 stepPx=px;
                float2 spread=px;
                float2 bestFlow=flow; best=1e20; second=1e20;
                [unroll] for(int y=-1;y<=1;y++) [unroll] for(int x=-1;x<=1;x++){
                    float2 cand=flow+float2((float)x,(float)y)*stepPx;
                    float err=patch_error_lvl(cuv,cuv+cand,spread);
                    if(err<best){ second=best; best=err; bestFlow=cand; }
                    else if(err<second) second=err;
                }
                flow=bestFlow;
            }
            float ambiguity=saturate((second-best)*4.0);
            float quality=1.0-saturate(best*1.6);
            float conf=saturate(quality*(0.35+0.65*ambiguity));
            // Store flow in pixels so the existing interpolation path can consume it.
            return float4(flow/px,conf,1);
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
            float4 plain=lerp(texPrev.SampleLevel(smp,i.uv,0),texCurr.SampleLevel(smp,i.uv,0),0.5);
            float4 warped=lerp(a,b,0.5);
            return lerp(plain,warped,w);
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
        if(g_gen_rtv){g_gen_rtv->Release();g_gen_rtv=nullptr;} if(g_gen_tex){g_gen_tex->Release();g_gen_tex=nullptr;}
        g_generated_ready=false;
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
    bool mk_generated(const D3D11_TEXTURE2D_DESC& bb){
        D3D11_TEXTURE2D_DESC d=bb;
        d.Usage=D3D11_USAGE_DEFAULT;
        d.BindFlags=D3D11_BIND_RENDER_TARGET;
        d.CPUAccessFlags=0; d.MiscFlags=0; d.SampleDesc.Count=1; d.SampleDesc.Quality=0;
        if(FAILED(g_dev->CreateTexture2D(&d,nullptr,&g_gen_tex))) return false;
        return SUCCEEDED(g_dev->CreateRenderTargetView(g_gen_tex,nullptr,&g_gen_rtv));
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
        if(!mk_generated(d))return false;
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
        QueryPerformanceFrequency(&g_qpc_freq);
        g_pacing_enabled = true;
        wchar_t paceBuf[16]{};
        if (GetEnvironmentVariableW(L"FSRINJ_DX11_PACING", paceBuf, 16) > 0 && paceBuf[0] == L'0')
            g_pacing_enabled = false;
        g_ready=true; LOGF("[fg] initialized (pyramid optical flow + smoothing + paced generated present)"); return true;
    }

    void note_real_present_timing(){
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        if(g_have_real_timing && g_qpc_freq.QuadPart){
            double dt=double(now.QuadPart-g_last_real_present.QuadPart)/double(g_qpc_freq.QuadPart);
            if(dt>0.001 && dt<1.0) g_real_interval_sec = g_real_interval_sec>0.0 ? (g_real_interval_sec*0.90 + dt*0.10) : dt;
        }
        g_last_real_present=now; g_have_real_timing=true;
    }
    void pace_generated_present(){
        if(!g_pacing_enabled || !g_have_real_timing || g_real_interval_sec<=0.0 || !g_qpc_freq.QuadPart) return;
        double target=double(g_pace_fraction)*g_real_interval_sec;
        double cap=0.90*g_real_interval_sec;
        if(target>cap) target=cap;
        LARGE_INTEGER now{}; int guard=0;
        for(;;){
            QueryPerformanceCounter(&now);
            double elapsed=double(now.QuadPart-g_last_real_present.QuadPart)/double(g_qpc_freq.QuadPart);
            if(elapsed>=target || ++guard>2000000) break;
            if(target-elapsed>0.0015) Sleep(0); else YieldProcessor();
        }
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

    void draw_interpolated_to(ID3D11RenderTargetView* target){
        if(!target) return;
        SB s; save(s);
        const FLOAT bf[4]={0,0,0,0}; g_ctx->OMSetBlendState(g_blend,bf,0xffffffff);
        g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs,nullptr,0); g_ctx->PSSetSamplers(0,1,&g_smp); g_ctx->PSSetConstantBuffers(0,1,&g_cb);
        ID3D11ShaderResourceView* nul=nullptr;
        pass(g_flow1_rtv,(float)g_lw,(float)g_lh,g_ps_flow,  g_prev_srv,g_curr_srv,nul);       // raw pyramid flow
        pass(g_flow2_rtv,(float)g_lw,(float)g_lh,g_ps_smooth,nul,nul,g_flow1_srv);             // smoothed flow

        // Optional depth-assisted disocclusion (default off; only if a readable depth exists).
        ID3D11ShaderResourceView* dsrv = core::config().use_depth.load() ? depth::current_srv() : nullptr;
        FlowCB cb{g_w,g_h,g_lw,g_lh,1.f/g_w,1.f/g_h,kSearchR,kSearchS,kPatchP,kDS, dsrv?1:0, 0};
        g_ctx->UpdateSubresource(g_cb,0,nullptr,&cb,0,0);
        g_ctx->PSSetShaderResources(3,1,&dsrv);

        pass(target,(float)g_w,(float)g_h,g_ps_interp, g_prev_srv,g_curr_srv,g_flow2_srv);      // interpolate
        ID3D11ShaderResourceView* nul4[4]={nullptr,nullptr,nullptr,nullptr}; g_ctx->PSSetShaderResources(0,4,nul4);
        restore(s);
    }

}

void before_present(IDXGISwapChain* sc, PresentTrampoline, unsigned){
    g_real.fetch_add(1,std::memory_order_relaxed);
    if(!core::config().framegen_enabled.load()){ g_have_prev=false; g_generated_ready=false; return; }
    if(!lazy(sc)) return;
    ID3D11Texture2D* bb=nullptr;
    if(FAILED(sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb))||!bb) return;
    if(!ensure(bb)){ bb->Release(); return; }
    g_ctx->CopyResource(g_curr_tex,bb);
    if(g_have_prev && g_gen_rtv){
        draw_interpolated_to(g_gen_rtv);
        g_generated_ready=true;
    } else {
        g_generated_ready=false;
    }
    std::swap(g_prev_tex,g_curr_tex); std::swap(g_prev_srv,g_curr_srv);
    g_have_prev=true; bb->Release();
}

void after_present(IDXGISwapChain* sc, PresentTrampoline present, unsigned flags){
    note_real_present_timing();
    if(!core::config().framegen_enabled.load() || !present || !g_generated_ready || g_inside_generated_present || !g_gen_tex) return;
    ID3D11Texture2D* bb=nullptr;
    if(FAILED(sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb))||!bb) return;
    if(g_pacing_enabled) pace_generated_present();
    g_ctx->CopyResource(bb,g_gen_tex);
    g_inside_generated_present=true;
    HRESULT hr=present(sc,0,flags);
    g_inside_generated_present=false;
    bb->Release();
    g_generated_ready=false;
    if(SUCCEEDED(hr)) g_gen.fetch_add(1,std::memory_order_relaxed);
    else LOGF("[fg] generated Present failed hr=0x%08lX", hr);
}
void on_resize(){ rel(); }
void shutdown(){ rel();
    if(g_cb)g_cb->Release(); if(g_blend)g_blend->Release(); if(g_smp)g_smp->Release();
    if(g_ps_interp)g_ps_interp->Release(); if(g_ps_smooth)g_ps_smooth->Release(); if(g_ps_flow)g_ps_flow->Release(); if(g_vs)g_vs->Release();
    if(g_ctx)g_ctx->Release(); if(g_dev)g_dev->Release(); g_ready=false; }
uint64_t real_frames(){ return g_real.load(std::memory_order_relaxed); }
uint64_t generated_frames(){ return g_gen.load(std::memory_order_relaxed); }

} // namespace framegen
