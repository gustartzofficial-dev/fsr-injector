#pragma once
struct IDXGISwapChain;

// The ImGui overlay + FSR control menu. Driven from the Present hook.
namespace overlay {

void on_present(IDXGISwapChain* sc);   // called every frame from hk_Present
void on_resize_buffers();              // release RTV before swapchain resize
void on_after_resize(IDXGISwapChain* sc);
void shutdown();

} // namespace overlay
