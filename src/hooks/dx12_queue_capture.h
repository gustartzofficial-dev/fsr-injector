#pragma once

struct IDXGIFactory;
struct IDXGISwapChain;
struct ID3D12CommandQueue;
struct IUnknown;

namespace hooks::dx12 {

// Installs hooks on a DXGI factory returned by CreateDXGIFactory*. These hooks
// observe CreateSwapChain* calls and remember the ID3D12CommandQueue used by
// each DX12 swapchain. A DX12 Present hook alone cannot recover the queue.
bool install_factory_hooks(IUnknown* factory_unknown);

// Returns an AddRef'd queue for the swapchain, or nullptr if this is not a DX12
// swapchain / we did not observe its creation.
ID3D12CommandQueue* queue_for_swapchain(IDXGISwapChain* sc);

// Called during DLL detach to release captured queues.
void shutdown();

} // namespace hooks::dx12
