#pragma once
struct IDXGISwapChain;

namespace overlay::dx12 {

bool on_present(IDXGISwapChain* sc);
void on_resize_buffers();
void on_after_resize(IDXGISwapChain* sc);
void shutdown();

} // namespace overlay::dx12
