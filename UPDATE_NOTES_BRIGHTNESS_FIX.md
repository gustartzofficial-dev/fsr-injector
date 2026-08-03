# Update notes — DX11 random brightening fix (state leakage)

Reported: with the injector's effects active in Tales of Arise (DX11), the
image randomly gets brighter, as if a brightness filter were applied. Frame
generation on its own looked fine.

## Root cause

The DX11 sharpener never set — or restored — blend, depth-stencil, or
rasterizer state. It bound its shaders and drew a fullscreen triangle using
**whatever pipeline state the game happened to leave bound at Present time**.

When the game's last draw left an additive or alpha blend state active, our
sharpened quad was *blended onto* the backbuffer instead of replacing it.
Image + image = a brighter image. Because the leftover state depends on
whichever draw call happened to be last that frame, the effect appeared and
disappeared unpredictably — matching the "random brightening" report exactly.

This also explains why frame generation alone looked correct: that path
already created an explicit opaque blend state and saved/restored the game's.
The sharpener did neither. The two paths were inconsistent, and the sharpener
was the broken one.

A second, related hazard existed on both paths: a leftover **scissor rect** or
**depth test** from the game can clip or reject a fullscreen triangle, which
would show up as the effect applying to only part of the screen or vanishing
in certain scenes.

## The fix

**`src/fsr/upscaler.cpp` (DX11 sharpener)**

- Creates explicit state objects at init: opaque blend with full RGBA write,
  depth test/write disabled, solid fill with culling and scissor off.
- Binds all three before its draw, so the game's state is never inherited.
- The state block now saves and restores blend state (+ blend factor and
  sample mask), depth-stencil state (+ stencil ref), rasterizer state, and
  scissor rects, in addition to what it already saved.

**`src/fsr/framegen.cpp` (DX11 frame generation)**

Hardened the same way for consistency: it already handled blend state, and now
also forces and restores depth-stencil, rasterizer, and scissor state.

Restoration matters as much as setting: UE-era engines cache render state in
their RHI and skip redundant sets, so state we change behind the engine's back
is not necessarily re-set by the game, and can corrupt *subsequent* frames.

## What to expect

The brightening should be gone with the sharpener active. If any brightness
shift remains, it is a different mechanism (a color-space/sRGB issue rather
than a blend issue) — say so and note whether it is constant or intermittent,
since a constant shift points at sRGB encode/decode and an intermittent one
points at remaining state leakage.

Build stamp for this revision: `0.5.2-dx11-state-fix` (shown in the menu and
in the first line of the log).
