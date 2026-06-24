# FSR Injector — Sonic Frontiers Edition

A DirectX 11 graphics-injection mod dedicated to **Sonic Frontiers** (Hedgehog Engine 2),
adding FSR-style upscaling/sharpening and **experimental frame generation** to a game that
ships with no native upscaler, no frame generation, and a 60 FPS cap.

It loads as a `dxgi.dll` proxy placed next to the game executable and hooks the DXGI present
path to insert its own post-process and (experimental) generated frames.

This is a **single-game fork** of the general-purpose
[`fsr-injector`](https://github.com/gustartzofficial-dev/fsr-injector) project. The point of
narrowing to one game is to replace the original's *heuristic* resource detection with
**game-specific fingerprints** for Hedgehog Engine 2's depth and motion-vector buffers —
deterministic detection instead of guessing.

> **Status: experimental research project.** It is not a drop-in FSR3 replacement. See
> [Honest limitations](#honest-limitations) before expecting native-quality results.

## Target

- **Game:** Sonic Frontiers
- **Engine:** Hedgehog Engine 2
- **API:** DirectX 11
- **Why:** no native FSR/DLSS/XeSS, no frame generation, hard 60 FPS cap

The inherited DX12 code paths from the base project are retained but are **not** the focus
here, since Frontiers is a DX11 title.

## How it works

Frame generation wants four inputs. This fork's job is to obtain each one from a DX11 game
that was never built to expose them:

1. **Color** — captured from the swapchain backbuffer via the Present hook. (Done.)
2. **Depth** — located by tracking depth-stencil binds and matching the full-resolution depth
   target (`D32_FLOAT` / `R32_TYPELESS` / `D24S8`). If it is not SRV-bindable, a private
   `CopyResource` copy is made. Used for disocclusion and reprojection.
3. **Motion vectors** — the hard part, with two routes:
   - **Engine velocity buffer (high quality):** Hedgehog Engine 2 uses TAA, so it produces a
     per-pixel motion buffer each frame (typically full-res `RG16F`, written just before the
     TAA resolve). Fingerprinting that target yields *true* motion vectors and is the path to
     FSR3-class quality.
   - **Optical-flow fallback (game-agnostic):** estimate motion from consecutive color frames
     (depth-guided), the same idea as Lossless Scaling / AFMF. Works with no engine data but
     has more UI ghosting and disocclusion error.
4. **Camera / jitter (optional):** view/projection matrices and TAA jitter, sniffable from
   constant buffers. Usually unnecessary once real motion vectors are available.

Frame generation itself is **interpolation**: the latest real frame is held back, an
intermediate frame is synthesized by warping between the previous and current frames using the
motion/flow field, then the interpolated frame and the real frame are presented with pacing.

### Finding the buffers (do this first)

The intended workflow is **RenderDoc-first**: capture a frame of Sonic Frontiers, walk it to
locate the depth pass, the velocity target (full-res `RG16F` before TAA), and the TAA resolve,
and record each resource's format, dimensions, bind flags, and draw-order position. Those
become the fingerprints that replace the heuristic scout. Identify the buffers before writing
detection code.

## Controls

Temporary test controls; to be replaced by a proper clickable menu.

- `Home` — show/hide the overlay menu.
- `End` — enable/disable the post-process (sharpen/upscale) pass.
- `PageUp` / `PageDown` — increase/decrease sharpness.
- `Insert` / `Delete` — increase/decrease internal render scale.
- `F1` / `F2` / `F3` — quality / balanced / performance presets.
- `F4` — motion/interpolation preview.
- `F5` — experimental generated-frame presentation.
- `F6` / `F7` — scout motion-vector validation / use (staged; experimental).

## Runtime environment options

DX11 path (relevant for Frontiers):

```bat
set FSRINJ_DX11_PACING=0        :: disable generated-frame pacing
set FSRINJ_DX11_MENU_IN_GEN=0   :: do not draw the menu into generated frames
```

Inherited DX12 flags (`FSRINJ_DX12_SHARPEN`, `FSRINJ_DX12_SHARPNESS`, `FSRINJ_DX12_SCALE`,
`FSRINJ_DX12_GENPRESENT`, `FSRINJ_DX12_SCOUT_MV`, `FSRINJ_DX12_IMGUI`) still exist but only
affect the DX12 code path.

## DX11 feature status

The DX11 path carries the core test features:

- pyramid / coarse-to-fine optical-flow frame generation
- generated-frame pacing toggle
- adaptive sharpener
- depth-assisted disocclusion toggle
- private-copy fallback for DSV-only depth buffers
- overlay/menu drawn into generated frames by default to reduce flicker

## Honest limitations

- **"FSR3 levels" is aspirational.** AMD's official FSR3 frame generation is DX12-oriented and
  expects real motion vectors, depth, and reactive masks. On a DX11 game with no native
  inputs, the realistically achievable target is AFMF / Lossless-Scaling-class frame gen —
  pushed *toward* FSR3 quality only if Hedgehog Engine 2's true velocity buffer is successfully
  extracted. Optical-flow-only output will always show UI and disocclusion artifacts.
- **You need a high-refresh display.** Generating 60 → 120 frames only helps on a 120/144 Hz
  monitor. On 60 Hz you cannot present faster than refresh, so frame gen would only add latency.
- **The 60 FPS cap.** For interpolation you do not strictly need to unlock it (render 60 real,
  interpolate to 120), but check *how* Frontiers caps — internal limiter vs vsync — so generated
  presents are not throttled in the present path.
- **Latency.** Interpolation holds a frame back, costing roughly half a frame to a full frame
  of latency. Smoothness improves; input latency stays at the 60 FPS cadence.
- **UI/HUD.** Screen-space UI must not be warped or it ghosts. A per-game fork can fingerprint
  the UI pass to mask it; until then expect HUD artifacts.

## Roadmap

1. RenderDoc capture of Sonic Frontiers; identify depth + velocity + TAA-resolve resources.
2. Replace the heuristic scout with Hedgehog Engine 2 fingerprints (deterministic detection).
3. Validated safe-copy of the depth and velocity buffers into SRV-capable private textures.
4. Feed real depth + motion vectors into the interpolation pass.
5. UI-pass fingerprinting and masking to stop HUD ghosting.
6. Improve generated-frame pacing/quality and handle the 60 FPS cap cleanly.
7. Replace temporary hotkeys with a proper clickable menu/preset UI.
8. Evaluate deeper FidelityFX SDK integration once real inputs and pacing are stable.

## Building

Build on a Windows runner via GitHub Actions; workflow files live under `.github/workflows/`.
After the build completes, download the `dxgi.dll` artifact and place it next to the Sonic
Frontiers executable. (The output is named `dxgi.dll` regardless of the project/fork name.)

## Credits & lineage

Forked from the general-purpose `fsr-injector`. Generic engine-agnostic improvements are still
pulled from that upstream; this repository adds the Sonic Frontiers / Hedgehog Engine 2
specialization on top.

## Legal

This is an unofficial, fan-made graphics mod. It is not affiliated with or endorsed by SEGA or
Sonic Team. Sonic Frontiers is a trademark of its respective owners. Use at your own risk.
