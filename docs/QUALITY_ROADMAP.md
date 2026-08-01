# Quality roadmap: beating flat-frame capture tools

This document plans the two upgrades that were deliberately not implemented
blind in this pass, because both need in-game iteration to tune: the dedicated
present thread, and FidelityFX Optical Flow / Frame Interpolation adoption.
The real FSR 1.0 (EASU+RCAS) upscaler is already integrated -- see
UPDATE_NOTES_FSR1.md.

## Why these two, in this order

Tools like Lossless Scaling win today on (a) frame pacing polish and (b) a
mature optical flow implementation. They cannot win on information: they never
see depth, candidate motion vectors, or the pre-present pipeline. Closing (a)
and (b) while keeping in-process access is the whole strategy.

## Stage 1 -- Dedicated present thread (pacing)

Current state: generated frames are paced by blocking the game's present
thread (waitable timer + spin in `pace_generated_present`). Costs up to half a
frame of game CPU time, and a game hitch poisons the pacing EMA.

Target architecture:

- The Present hook no longer presents the generated frame. It only: copies the
  backbuffer, signals a fence, records the QPC timestamp, and returns.
- A dedicated thread owns generated-frame presentation: it waits on the fence,
  sleeps until the temporal midpoint (predicted from the EMA of real-present
  intervals), copies `g_generated` into the current backbuffer, and calls the
  Present trampoline itself.
- Synchronization: the thread and the hook must never touch the swapchain
  concurrently. A small state machine (IDLE -> GEN_READY -> PRESENTING) with a
  mutex + condition variable is enough; the hook skips its work if the thread
  is mid-present (never blocks the game).
- Shutdown: the thread must be joined before `release_frame_resources`, and
  the device-lost flag must short-circuit it.

Risks to test in-game: swapchains in exclusive fullscreen (Present from a
second thread is legal for DXGI but some drivers behave differently), games
that call Present from multiple threads themselves, and interaction with the
existing `g_inside_generated_present` recursion guard (which becomes
thread-id-based).

Expected win: judder elimination -- typically *feels* like a bigger upgrade
than better interpolation math.

## Stage 2 -- FidelityFX Optical Flow + Frame Interpolation

The FidelityFX SDK (MIT) ships the exact components FSR3 frame generation is
built from, usable standalone:

- `FidelityFX-SDK/sdk/src/components/opticalflow` -- luminance-pyramid block
  search in optimized compute, designed to work WITHOUT engine motion vectors.
  This replaces our hand-rolled SAD pyramid in one step.
- `FidelityFX-SDK/sdk/src/components/frameinterpolation` -- real disocclusion
  logic, inpainting, and per-pixel confidence, consuming optical flow (and
  optionally depth + motion vectors -- which is where the scout's validated
  candidates plug in later).

Integration plan:

1. Vendor the SDK's ffx-api DX12 backend (or the prebuilt ffx_backend_dx12
   static lib) under third_party/. It manages its own descriptors/heaps, so it
   coexists with our minimal pipeline.
2. Create the OpticalFlow context at swapchain size on init; each frame,
   dispatch it with (previous, current) captured backbuffers; it outputs an
   optical flow vector texture + scene-change detection.
3. Feed FrameInterpolation with: current + previous color, optical flow, and
   (when the scout has a validated candidate) depth. Output replaces
   `g_generated`.
4. Keep the existing shader interpolation as the fallback path exactly like
   the legacy-vs-real-FSR1 split done in this update (runtime capability
   check, graceful degradation, log line stating which path is live).
5. Only after 1-4 are stable: evaluate full FSR3 FG pacing integration, which
   wants to own presentation (aligns with Stage 1's thread).

Sizing: Stage 1 is roughly a week of focused work plus per-game testing.
Stage 2 is the big one -- expect the backend integration (memory, barriers,
format conversions for non-RGBA8 swapchains) to dominate the effort.

## Smaller follow-ups (any order)

- HDR: detect scRGB/HDR10 colorspaces via IDXGISwapChain3 and either
  linearize before RCAS or disable with a log line.
- History ping-pong to drop one full-res copy per frame.
- DX11 parity for the real FSR1 shaders (the same embedded headers compile
  under SM5 for D3D11; only the binding code differs).
