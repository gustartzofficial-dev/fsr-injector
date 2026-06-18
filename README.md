# FSR Injector (working title)

A ReShade-style **DLL-proxy injector** that drops into a game's folder and adds an
in-game overlay menu for enabling **FSR upscaling** and **FSR frame generation** —
either by *hijacking* an upscaler the game already ships (DLSS / XeSS / FSR2/3), or
falling back to post-process scaling + optical-flow interpolation when the game has
no upscaler at all.

This repo is being built in phases. **Read the Status section before assuming a
module works.**

---

## How it injects (the ReShade model)

The DLL is named after a system library the game loads early — `dxgi.dll` is the
target here — and placed next to the game executable. Windows' default search order
loads the local `dxgi.dll` *before* the real one in `System32`. Our DLL:

1. Loads the **real** `System32\dxgi.dll` and resolves its exports.
2. Re-exports the same symbols, forwarding each call straight through, so the game
   behaves exactly as if it loaded the real DXGI.
3. Separately installs a **Present hook** (via MinHook, vtable-based) so we get a
   callback every frame to draw the overlay and, later, run FSR.

We deliberately do **not** hand-write full COM wrappers for every DXGI interface.
The proxy exists only to (a) load early and (b) host the hook. Interception happens
through the Present/ResizeBuffers vtable hook — robust and far less error-prone than
wrapping `IDXGISwapChain4` by hand.

> Install: copy the built `dxgi.dll` next to the game `.exe`. To uninstall, delete it.

---

## Architecture

```
dllmain.cpp            DLL entry. Proxy bootstrap + forwarded dxgi exports.
proxy/                 Resolves and forwards the real dxgi.dll exports.
hooks/                 MinHook setup; Present + ResizeBuffers detours.
overlay/               Dear ImGui overlay + the FSR control menu.
detect/                Scans loaded modules to classify the game (DLSS/XeSS/FSR/bare).
fsr/                   Integration boundary to the FidelityFX SDK (interface + stubs).
core/                  Config struct, logging.
```

Frame path (target end state):

```
game renders frame --> [hijack point OR backbuffer] --> FSR upscale (D3D11)
                                                          |
                                          (frame gen, D3D12 interop) --> present
overlay drawn on top --> Present (real)
```

---

## Auto-detect (chosen design: "Both, with auto-detect")

At startup, `detect/` inspects loaded modules and known signatures to classify:

- **nvngx.dll / nvngx_dlss** present  -> game supports DLSS  -> hijack path
- **libxess.dll**                     -> game supports XeSS  -> hijack path
- **ffx_*.dll / amd_fidelityfx***     -> game supports FSR2/3 -> upgrade/passthrough
- none of the above                   -> **bare game** -> post-process fallback

Hijacking gives the best quality because the game already feeds the upscaler proper
motion vectors + depth. The bare-game path can only do spatial upscaling + optical-
flow frame gen (Lossless-Scaling tier), since no motion vectors exist.

---

## Graphics API: D3D11 first

- **Upscaling on D3D11:** native FidelityFX DX11 backend. Feasible directly.
- **Frame generation on D3D11:** FSR FG is **DX12-native**. Requires creating a
  D3D12 device, sharing frames via shared NT handles (D3D11<->D3D12 interop), running
  interpolation on D3D12, presenting through a D3D12 proxy swapchain. This is the
  hardest module and is scheduled last.

ML FSR FG 4.0 is RDNA4 + DX12 + Win11 only, so the broad target is the **analytical
FSR 3.1.x** frame-gen path.

---

## Build

Requires: Windows, Visual Studio 2022 (or clang-cl), CMake >= 3.26, Windows SDK.
Dependencies are fetched by CMake: **MinHook** and **Dear ImGui**.
The **FidelityFX SDK** must be cloned separately (see `fsr/README` once that phase
starts) because of its build step.

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/dxgi.dll` — copy next to the target game's executable.

---

## Status

| Module                         | State            |
|--------------------------------|------------------|
| Proxy bootstrap + forwarding   | foundation done  |
| Present / ResizeBuffers hook   | foundation done  |
| ImGui overlay + menu shell     | foundation done  |
| Upscaler auto-detect           | foundation done  |
| FSR upscaling (D3D11)          | interface only   |
| FSR frame gen (D3D11<->D3D12)  | interface only   |
| Post-process fallback          | not started      |

**This is phase 1.** The injection harness, overlay, and detection are real and
buildable. The FSR boundary is defined as an interface (`fsr/fsr_integration.h`)
with honest stubs — wiring it to the FidelityFX SDK is the next phase and needs a
GPU + iteration to get right. Nothing here has been compiled or run on hardware by
the author of the scaffold; treat the COM/D3D specifics as needing a first build pass.


### DX12 RCAS-style sharpening

The DX12 path now has a fullscreen RCAS-style sharpening pass. This is not full FSR upscaling yet; it is the sharpening stage built on the working DX12 backbuffer post-process path. Use `FSRINJ_DX12_SHARPNESS=0.0..1.0` to tune strength, or `FSRINJ_DX12_SHARPEN=0` to disable it.


## DX12 native settings overlay update

Suggested commit name: `Add native DX12 settings overlay`

This build keeps Dear ImGui bypassed for DX12 and draws a lightweight native D3D12 overlay directly inside the working EASU/RCAS fullscreen pass. The overlay shows the DX12 UI header, post-process status, scale, sharpness, and a small scale bar. This follows the safer path used by mature DX12 overlays: separate capture/render backend from UI state, keep per-frame synchronization, and avoid the ImGui DX12 backend until descriptor/input issues are isolated.

### DX12 native menu controls

The DX12 backend uses a lightweight native overlay instead of Dear ImGui.

- Home: show/hide the native menu.
- End: enable/disable the DX12 EASU/RCAS post-process effect.
- PageUp/PageDown: adjust sharpness.
- Insert/Delete: adjust internal test scale.
- F1/F2/F3: Quality/Balanced/Performance presets.

The menu should remain visible while the post-process effect is on or off. The post-process effect and menu visibility are separate states.

### DX12 generated-frame experiment

The DX12 path now includes an experimental generated-frame presentation mode. Press F5 to toggle it. F4 remains the safer interpolation preview mode that blends history into the real frame without inserting an extra Present.

Suggested commit name for this update: `Add FSR1 pass and experimental generated frame presentation`

### DX12 native menu FPS readout

The DX12 native overlay now shows two FPS counters:

- `FPS GAME`: the real game frame rate reaching the injector.
- `FPS OUT`: estimated output present rate, including experimental generated-frame Presents when F5 is enabled.

Controls:

- `Home`: show/hide native menu
- `End`: toggle DX12 post-process
- `F4`: preview interpolation inside real frames
- `F5`: experimental generated-frame presentation

### Generic Resource Scout v1

The DX12 scout now hooks command queues, command lists, RTV/DSV descriptor creation, render-target binding, barriers, and draw calls. It reports candidate depth and motion-vector-like resources in the log and menu. This is a discovery layer only; candidates are not yet consumed by the frame-generation shader.

### Scout v2: command-list tracking

Suggested commit name: `Add DX12 command-list scout tracking`

This build adds DX12 command-list hook coverage so the generic scout can begin seeing how the game uses render targets/depth buffers during command recording. Look for `[scout-dx12]` lines in `fsr-injector.log` after a short time in-game.

### Scout v2.1 diagnostics

The generic resource scout now logs DX12 frame summaries from Present and adds broader DX11 render-flow hooks. This helps compare DX12 and DX11 games with the same diagnostic language before detected buffers are used for frame generation.
