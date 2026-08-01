# Update notes — multi-frame generation (2x / 3x / 4x)

DLSS-MFG-style multi-frame generation for the DX12 path. Instead of a single
generated frame at the temporal midpoint, the injector now renders up to three
generated frames per real frame, placed at t = k/N between the previous and
current real frames, and presents each in its own paced slot.

Compile-verified with a clean MinGW-w64 cross-build.

## How it works

- **Three generated-frame textures** (`g_gen[3]`) with their own RTVs and
  resource-state tracking replace the single `g_generated` target.
- **The interpolation shader is now time-parameterized.** `interpT` was added
  to the root constants; `fsr3_lite_interpolate` uses it to warp the previous
  frame by `flow * t` and the current frame by `flow * (1 - t)`, then blends at
  `t`. Previously all three were hardcoded to 0.5. At 4x the three frames are
  generated at t = 0.25, 0.50, 0.75.
- **Presentation loops** over the ready frames, pacing each to its own
  fraction of the measured real-frame interval, so frames land evenly in time
  rather than in a burst. 2x still uses the user-tunable
  `FSRINJ_DX12_PACE_FRACTION` (default midpoint); 3x/4x use even k/N spacing.
- All existing guards still apply: waitable-swapchain gating (and the
  `FSRINJ_DX12_GENPRESENT_FORCE` override), device-lost passivity, and the
  recursion guard.

## Controls

- **F8** cycles the multiplier 2x -> 3x -> 4x (logged each press).
- The ImGui menu has an **FG multiplier** dropdown next to the frame-generation
  checkbox.
- **INI / env:** `FSRINJ_DX12_GENMULT = 2` (accepts 2, 3, or 4) sets the
  startup multiplier.

Changing the multiplier drops any already-rendered generated frames for that
cycle, so temporal spacing never mixes multipliers mid-flight.

## What to expect, honestly

At 30 fps locked, 4x should read game 30 / output ~120 in the overlay. Two
caveats worth testing for:

1. **Artifacts scale with t distance from a real frame.** The t=0.25 and
   t=0.75 frames warp further along the flow vector than a midpoint frame, so
   any flow error is proportionally more visible. Expect 4x to look worse per
   frame than 2x — this is true of every interpolation-based FG, and is why
   quality of the flow estimate matters more as N rises.
2. **CPU cost on the game thread.** Pacing still blocks the game's present
   thread (see docs/QUALITY_ROADMAP.md, Stage 1). At 4x on a 33 ms frame, the
   injector holds that thread across three wait points before returning. If
   the game's own framerate sags in 4x but is fine at 2x, that is this
   limitation, and the dedicated present thread is the fix — multi-frame
   generation is precisely the workload that makes that refactor worth doing.

The GPU cost is also real: each generated frame is a full-resolution
interpolation pass, so 4x runs three of them per real frame.
