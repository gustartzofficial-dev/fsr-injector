# Update notes — DX11 parity pass

The previous three updates (real FSR1, waitable-swapchain guard + force
override, multi-frame generation) all landed in the DX12 backend only. This
update brings the DX11 path up to the same level. Clean cross-build verified.

## 1. Real FidelityFX RCAS on DX11 (`src/fsr/upscaler.cpp`)

The DX11 sharpener was a hand-rolled contrast-adaptive unsharp mask. It now
runs genuine AMD RCAS from the vendored `ffx_fsr1.h`, using the same embedded
headers and the same CPU-side constant packing as DX12.

Note on scope: the DX11 path sharpens the finished backbuffer at native
resolution and has no render-scale pipeline, so there is nothing for EASU to
upscale from — RCAS is the correct and complete FSR1 component here. (Adding a
true DX11 render-scale path would require intercepting the game's own render
targets, which is a much larger change and is listed in the roadmap.)

Same fallback policy as DX12: the shader compiles with the FidelityFX includes
first, and silently falls back to the legacy sharpener if that fails. The log
states which is live:
`[up] DX11 sharpener initialized (AMD FidelityFX RCAS | legacy contrast-adaptive)`

## 2. Waitable-swapchain guard on DX11 (`src/fsr/framegen.cpp`)

**This was a real asymmetry bug.** DX11 framegen also issues an extra
`present()` per frame, but had no waitable-swapchain check — so DX11 games
using a frame-latency waitable object could stall exactly the way DX12 ones
would have. The guard now exists on both paths, with the same force override.

## 3. Multi-frame generation on DX11 (2x / 3x / 4x)

The DX11 interpolation shader had `0.5` hardcoded in three places. It is now
driven by an `interpT` constant (replacing the CB's unused pad field, so
alignment is unchanged), and `before_present` renders and presents N-1 frames
at t = k/N, matching the DX12 implementation.

## 4. Focus-gated menu hotkey on DX11 (`src/overlay/overlay.cpp`)

The DX11 menu toggle now only fires while the game window is foreground, same
as DX12. Edge state still updates in the background so refocusing never
replays a press.

## 5. API-neutral setting keys

Because the multiplier and force-override keys now drive both backends, they
are readable under neutral names, which take priority when present:

- `FSRINJ_GENMULT` (falls back to `FSRINJ_DX12_GENMULT`)
- `FSRINJ_GENPRESENT_FORCE` (falls back to `FSRINJ_DX12_GENPRESENT_FORCE`)

Existing INIs using the `FSRINJ_DX12_*` names keep working unchanged.

## Shared refactor

The FidelityFX `ID3DInclude` handler moved from `overlay_dx12.cpp` into
`src/fsr/ffx_include.h` so both backends serve the embedded headers from one
implementation.

## Remaining DX11/DX12 differences (intentional, documented)

- **No render-scale upscaling on DX11** — the DX11 path sharpens only; there
  is no low-res render target to run EASU on. DX12 has the full scale
  pipeline (Insert/Delete, presets).
- **No F1–F8 hotkeys on DX11** — DX11 exposes its settings through the ImGui
  menu and INI rather than the DX12 path's function-key set.
- **No frame pacing on DX11** — generated frames are presented immediately
  after the interpolated draw rather than paced to a temporal midpoint. The
  dedicated present thread (roadmap Stage 1) is the right place to fix this
  for both backends at once.
