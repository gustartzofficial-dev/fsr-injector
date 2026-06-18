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

## DX12 crash safe-mode patch

Some DX12 games close immediately after the first DX12 overlay initialization if the injector submits its own ImGui command list without full fence/allocator/backbuffer-state tracking. The DX12 path now defaults to a safe mode:

- DX12 swapchain detection remains enabled.
- ID3D12CommandQueue capture remains enabled.
- DX12 overlay rendering is skipped by default.
- Set `FSRINJ_DX12_OVERLAY=1` before launching the game to opt into the experimental DX12 overlay renderer.

This is intentional. The next DX12 rendering patch should add proper per-frame fences, allocator retirement, and conservative backbuffer state handling before enabling overlay/upscaler rendering by default.

## DX12 overlay render stabilization patch

The previous safe-mode build disabled DX12 overlay drawing to prove that the crash was in command submission rather than in queue/swapchain capture. This patch re-enables DX12 overlay drawing by default and adds the missing synchronization pieces:

- one command allocator per swapchain backbuffer
- one fence value per backbuffer/frame context
- fence wait before resetting a command allocator
- queue signal after our overlay command list is submitted
- GPU idle wait before ResizeBuffers resource release and shutdown
- detailed HRESULT logging for DX12 overlay initialization and render failures

To temporarily disable only DX12 overlay drawing while keeping queue capture active, launch with:

```bat
set FSRINJ_DX12_OVERLAY=0
```

## DX12 staged overlay diagnostics

The DX12 overlay now skips the first Present that performs initialization and waits three additional Presents before attempting to render. This avoids submitting command lists while the game's swapchain is still settling and adds log markers before every risky D3D12 step:

- allocator reset
- command list reset
- PRESENT -> RENDER_TARGET barrier
- render target binding
- descriptor heap binding
- ImGui frame creation
- ImGui draw data recording
- RENDER_TARGET -> PRESENT barrier
- command list close
- ExecuteCommandLists
- fence signal

If a game still closes instantly, the last emitted `[overlay-dx12] step:` line identifies the failing stage.
