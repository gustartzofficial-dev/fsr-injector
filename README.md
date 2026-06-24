# FSR Injector Experimental

FSR Injector Experimental is a DXGI proxy / graphics-injection project for adding post-processing, upscaling experiments, and frame-generation research features to games that do not natively expose FSR/DLSS/XeSS integrations.

This project is currently experimental. It is not a production-quality FSR3 replacement yet, but it now has a working DX12 pipeline for image processing, generated-frame presentation experiments, and generic resource scouting.

## Current Status

### Working DX12 features

- DXGI proxy loading through `dxgi.dll` placement next to the game executable.
- DX12 swapchain detection and Present/Present1 hooks.
- DX12 command queue capture.
- Native non-ImGui DX12 overlay.
- FSR1-style EASU/RCAS-inspired upscale/post-process path.
- Runtime scale and sharpness controls.
- Frame history capture.
- Experimental motion/interpolation preview.
- Experimental generated-frame presentation.
- FPS/status display in the native overlay.
- Generic DX12 resource scouting hooks for command lists, render targets, depth targets, barriers, draw calls, and motion-vector-like candidates.

### Working DX11 features

- Existing DX11 overlay/path remains supported.
- Existing DX11 sharpening/framegen fallback path remains supported.
- Initial DX11 scout parity hooks are present for depth/render-resource diagnostics.

### Experimental / unstable features

- F5 generated-frame presentation is experimental.
- F6 scout motion-vector candidate usage is experimental.
- Generic motion-vector detection is heuristic-based and may select HDR/intermediate buffers instead of real engine velocity buffers.
- Full AMD FSR3 SDK frame generation is not integrated yet.

## Controls

Current test controls are temporary and will eventually be replaced by a proper clickable menu/preset UI.

- `Home` = show/hide native menu.
- `End` = enable/disable DX12 post-process.
- `PageUp/PageDown` = increase/decrease sharpness.
- `Insert/Delete` = increase/decrease internal test scale.
- `F1/F2/F3` = quality/balanced/performance presets.
- `F4` = motion/interpolation preview.
- `F5` = experimental generated-frame presentation.
- `F6` = experimental scout motion-vector candidate usage.

## Runtime environment options

- `FSRINJ_DX12_SHARPEN=0` disables the DX12 post-process path.
- `FSRINJ_DX12_SHARPNESS=<value>` sets startup sharpness.
- `FSRINJ_DX12_SCALE=<value>` sets startup internal scale.
- `FSRINJ_DX12_GENPRESENT=1` enables experimental generated-frame presentation at startup.
- `FSRINJ_DX12_SCOUT_MV=1` enables experimental scout motion-vector candidate usage at startup.

## What this project can do now

- Inject into DX12 games that load through a DXGI proxy.
- Modify the DX12 swapchain backbuffer safely.
- Apply a visible FSR1-style reconstruction/sharpening pass.
- Keep frame history.
- Present experimental generated frames.
- Scout for depth and motion-vector-like resources.

## What this project cannot do yet

- Guarantee native-quality FSR3 frame generation.
- Reliably obtain true engine motion vectors in every game.
- Separate UI/HUD from scene content generically.
- Use depth/motion candidates for production-quality interpolation.
- Replace official FSR3 integration in supported games.

## Generic resource scout

The generic scout is the current route for non-native games. It observes DX12/DX11 rendering behavior and tries to find useful resources such as depth buffers and velocity-like textures. This is different from OptiScaler-style API replacement, where the game already provides DLSS/FSR2/XeSS inputs.

The scout currently tracks:

- Render target views.
- Depth stencil views.
- Command-list draw calls.
- Resource barriers.
- Pipeline and root descriptor usage.
- Motion-vector-like candidates, especially full-resolution floating-point render targets.

The scout path is still heuristic. It should be treated as a discovery/debugging system first, not as guaranteed real motion-vector extraction.

## Roadmap

1. Stabilize scout motion-vector safe-copy and validation.
2. Add better candidate ranking and rejection.
3. Add depth candidate copying/visualization.
4. Feed validated depth/motion data into interpolation.
5. Improve generated-frame pacing and quality.
6. Replace temporary hotkeys with a proper clickable menu.
7. Investigate optional game/engine companion modules for higher-quality data when available.
8. Investigate deeper FidelityFX SDK / FSR3 integration once required inputs and pacing are stable.

## Building

Use GitHub Actions on a Windows runner. The repo includes workflow files under `.github/workflows/`. After the build completes, download the `dxgi.dll` artifact and place it next to the target game executable.

## Current Experimental Scout-MV Test Flow

The DX12 scout-MV path is intentionally staged:

1. Press `F6` to enable scout motion-vector validation. This only discovers a candidate and creates a private copy texture.
2. Press `F7` once to validate copying the candidate into the private texture.
3. Press `F7` again to actually use the copied motion-vector candidate in the shader.

If the game crashes after a specific stage, the last log line identifies whether the issue is candidate creation, copy/barrier validation, or shader sampling.


### DX11 parity status

The DX11 path now has the same core test features we validated on DX12:

- pyramid/coarse-to-fine optical-flow frame generation
- generated-frame pacing toggle
- adaptive sharpener
- depth-assisted disocclusion toggle
- private-copy fallback for DSV-only depth buffers
- overlay/menu drawn into generated frames by default to reduce flicker

Useful environment flags:

```bat
set FSRINJ_DX11_PACING=0
set FSRINJ_DX11_MENU_IN_GEN=0
```

