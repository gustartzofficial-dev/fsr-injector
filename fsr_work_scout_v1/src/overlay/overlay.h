#pragma once
struct IDXGISwapChain;
struct IDXGISwapChain1;
struct DXGI_PRESENT_PARAMETERS;

// The ImGui overlay + FSR control menu. Driven from the Present hook.
namespace overlay {

void on_present(IDXGISwapChain* sc);   // called every frame from hk_Present
using PresentFn = long (__stdcall*)(IDXGISwapChain*, unsigned int, unsigned int);
using Present1Fn = long (__stdcall*)(IDXGISwapChain1*, unsigned int, unsigned int, const DXGI_PRESENT_PARAMETERS*);
void after_present(IDXGISwapChain* sc, unsigned int flags, PresentFn present_fn);
void after_present1(IDXGISwapChain1* sc, unsigned int flags, const DXGI_PRESENT_PARAMETERS* pp, Present1Fn present1_fn);
void on_resize_buffers();              // release RTV before swapchain resize
void on_after_resize(IDXGISwapChain* sc);
void shutdown();

} // namespace overlay
