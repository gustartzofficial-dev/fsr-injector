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

## Patch: DX12 backbuffer sharpening pass

This patch replaces the native magenta DX12 marker with the first real DX12 image-processing feature.

Status:

- DX12 ImGui is still bypassed because the ImGui backend crashed at `ImGui new frame`.
- The DX12 command queue capture, command allocator, command list, barrier, RTV, and fence path remains active.
- A temporary copy of the current swapchain backbuffer is created every frame.
- A fullscreen triangle pixel shader samples that temporary texture and writes a simple sharpening pass back into the swapchain backbuffer.
- The Home key toggles the DX12 sharpen pass on/off through the existing overlay-visible config flag.

Environment variables:

- `FSRINJ_DX12_SHARPEN=0` disables the DX12 sharpen pass.
- `FSRINJ_DX12_SHARPNESS=<0.0-1.0>` overrides the initial DX12 sharpness value.

Expected log lines:

```text
[overlay-dx12] sharpen pass initialized ... enabled=on sharpness=...
[overlay-dx12] init-only present skipped; sharpen begins after warmup
[overlay-dx12] warmup present 1/3; skipping sharpen
[overlay-dx12] first DX12 sharpen frame submitted successfully
```

Next milestone if this works:

- Replace this simple sharpening shader with a FidelityFX-style RCAS pass.
- Then add an EASU/FSR1-style spatial upscaler path using the same backbuffer copy and fullscreen render infrastructure.


## Patch: DX12 RCAS-style sharpen pass

Suggested commit name: `Replace DX12 sharpen shader with RCAS-style pass`

This patch replaces the initial simple DX12 unsharp-mask shader with a more robust RCAS-style local-contrast sharpening pass. It keeps the already-working DX12 backbuffer copy and fullscreen render pipeline, but changes the pixel shader to use a local min/max limiter so sharpening is less halo-prone and closer to the FSR1 RCAS stage. Dear ImGui remains bypassed for DX12 while the graphics backend matures.

Runtime toggles:

- `Home`: toggles the DX12 sharpen pass on/off through the existing overlay-visible flag.
- `FSRINJ_DX12_SHARPEN=0`: disables the DX12 sharpen pass.
- `FSRINJ_DX12_SHARPNESS=0.0..1.0`: controls RCAS-style sharpness.


## DX12 native settings overlay update

Suggested commit name: `Add native DX12 settings overlay`

This build keeps Dear ImGui bypassed for DX12 and draws a lightweight native D3D12 overlay directly inside the working EASU/RCAS fullscreen pass. The overlay shows the DX12 UI header, post-process status, scale, sharpness, and a small scale bar. This follows the safer path used by mature DX12 overlays: separate capture/render backend from UI state, keep per-frame synchronization, and avoid the ImGui DX12 backend until descriptor/input issues are isolated.
