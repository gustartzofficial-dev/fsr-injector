# Update notes — real AMD FidelityFX FSR 1.0 integration (DX12)

The DX12 upscale path now runs AMD's actual FSR 1.0 algorithm instead of the
"EASU/RCAS-inspired" approximation. Verified with a full MinGW-w64 cross-build;
the HLSL side compiles at runtime and carries an automatic fallback (details
below), so a shader problem can never take the injector down.

## What changed

- **Vendored `third_party/ffx/`**: AMD's `ffx_a.h` + `ffx_fsr1.h` (MIT,
  license included) from the official GPUOpen FidelityFX-FSR repository.
- **Build-time embedding** (`cmake/embed_text_file.cmake`): the two headers are
  baked into the DLL as byte arrays and served to the runtime HLSL compiler
  through a custom `ID3DInclude` handler — no extra files next to the game.
- **CPU-side constants** (`src/fsr/fsr1_constants.{h,cpp}`): `FsrEasuCon` /
  `FsrRcasCon` computed by the *same* vendored header that runs on the GPU, so
  constant layouts can never drift. The 0–1 sharpness slider maps onto RCAS
  "stops" (1.0 → maximum sharpening).
- **New EASU pass** (`overlay_dx12.cpp`): a dedicated full-resolution pass
  renders genuine EASU from the low-res source into a new `g_easu` texture;
  the composite pass then applies genuine RCAS via `FsrRcasF` reading it.
  Plumbing: SRV table grew to 5 (t4 = EASU result), root constants to 24
  (RCAS uint4 + fsrActive flag), RTV heap +1, resource release/reset updated.
- **Graceful fallback**: the composite shader compiles twice-if-needed — first
  with `FSRINJ_REAL_FSR` and the FidelityFX includes, and if that fails for
  any reason, again as the old dependency-free approximation. The init log now
  states which path is live:
  `AMD FidelityFX FSR 1.0 (EASU+RCAS)` vs `legacy approximation`.

## What to expect in-game

Same controls, same behavior — but with the effect enabled you should see
visibly cleaner edge reconstruction at reduced scale (EASU's edge-directed
filtering vs the old gradient blend) and better-behaved sharpening (RCAS's
noise-aware limiter vs the old Laplacian). Check the log's init line to
confirm the real path engaged; if it says "legacy approximation", the
D3DCompile error above it is what I need to see.

## Also in this update

`docs/QUALITY_ROADMAP.md` — the concrete design for the next two quality
stages (dedicated present thread, then FidelityFX Optical Flow + Frame
Interpolation), including the risks that need in-game testing and why they
were not implemented blind.
