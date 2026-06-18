# DX12 Port Notes

This patch adds the first real DX12 bridge for the injector.

## What changed

- `CreateDXGIFactory`, `CreateDXGIFactory1`, and `CreateDXGIFactory2` now install factory hooks on the returned factory.
- The factory hooks observe `CreateSwapChain`, `CreateSwapChainForHwnd`, `CreateSwapChainForCoreWindow`, and `CreateSwapChainForComposition`.
- When the game creates a DX12 swapchain, the hook captures the `ID3D12CommandQueue` passed as the `pDevice` parameter and stores it by swapchain pointer.
- `overlay::on_present` now detects DX12 swapchains and routes them to a D3D12 ImGui backend instead of failing on `GetDevice(ID3D11Device)`.
- The old D3D11 framegen/sharpening path is now skipped for DX12 swapchains instead of repeatedly failing.

## What this enables

The DX12 overlay should now be able to render in DX12 games, provided the factory hooks see the swapchain creation before the first Present.
This is also the prerequisite for a proper DX12 FSR3/FSR3.1 frame-generation path, because FSR3 needs the device and command queue.

## What is not implemented yet

This does not yet implement true FSR3 frame generation or DX12 upscaling. The next step is to add a DX12 frame pipeline that:

1. Captures current/backbuffer resources with proper D3D12 barriers.
2. Maintains previous/current color history.
3. Adds depth and motion-vector discovery or a fallback optical-flow path.
4. Wires FidelityFX SDK contexts for FSR upscaling and frame interpolation.
5. Handles HUD-less/HUD-after paths if you want generated frames to avoid doubling UI.

## First test target

Use a simple DX12 sample/game first. Check the log for:

- `[dx12] hooked IDXGIFactory2::CreateSwapChainForHwnd`
- `[dx12] captured ID3D12CommandQueue ...`
- `[overlay] swapchain is DIRECTX 12, using D3D12 backend`
- `[overlay-dx12] initialized ...`

If the queue is not captured, the game may be creating the swapchain before the proxy has installed factory hooks, or through an unusual DXGI path that needs another hook.
