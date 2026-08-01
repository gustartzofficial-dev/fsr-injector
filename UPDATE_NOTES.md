# Update notes — stability, performance, and repo-hygiene pass

This is a bulk update. Every source change compiles cleanly (verified with a
MinGW-w64 x64 cross-build of the full project, MinHook pinned + ImGui v1.90.9).
No feature was removed; all existing hotkeys, env vars, and behaviors are kept.

## Crash / correctness fixes

- **Scout use-after-free fixed** (`src/capture/generic_resource_scout.cpp`).
  Descriptor entries previously stored raw `ID3D12Resource*` pointers and
  AddRef'd them later — a crash whenever the game destroyed a render target
  before the injector used the candidate (which games do constantly). Entries
  now hold a real reference (AddRef on store, Release on overwrite/evict), the
  map is bounded (4096 entries with oldest-first eviction), and the whole cache
  is reset on `ResizeBuffers` and shutdown. New API:
  `capture::scout::reset_dx12_resources()`.
- **`DXGI_PRESENT_TEST` handled** (`src/hooks/swapchain_hook.cpp`). Occlusion
  probes now pass straight through instead of running the sharpen/framegen/
  overlay pipeline on a present that never shows a frame.
- **Waitable swapchains detected** (`src/overlay/overlay_dx12.cpp`). On chains
  created with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, generated-
  frame presentation is disabled (hotkey + menu + startup flag) because the
  extra Present consumes the game's latency slots and stalls it. Upscaling and
  the overlay still work; the log explains why F5 is unavailable.
- **Fence timeout no longer corrupts the game** (`overlay_dx12.cpp`). A timed-
  out 2 s fence wait used to fall through and Reset in-flight allocators, which
  converts a GPU hiccup into `DXGI_ERROR_DEVICE_REMOVED` for the game. The wait
  result is now checked; on `GetDeviceRemovedReason() != S_OK` the injector goes
  permanently passive for the session instead.
- **Process-termination-safe `DllMain`** (`src/dllmain.cpp`). On
  `DLL_PROCESS_DETACH` with `lpReserved != nullptr` (process exiting), the DLL
  now only flushes the log. Running MinHook teardown / GPU releases /
  `FreeLibrary` under the loader lock while threads are already dead is a
  classic exit-time deadlock; the OS reclaims everything anyway.
- **Forwarded exports null-guarded** (`dllmain.cpp`, `dxgi_proxy.cpp`). If the
  real `dxgi.dll` failed to resolve, exports now retry once (thread-safe) and
  return `E_NOINTERFACE` instead of calling a null pointer.
- **Present1 → framegen trampoline guarded** (`swapchain_hook.cpp`), with a
  comment documenting the intentional cross-entry-point call.

## Performance fixes

- **No more mutex on every draw call.** The scout's per-draw / per-PSO / per-
  root-table / per-execute counters were serializing the game's entire command-
  recording thread pool through one lock, tens of thousands of times per frame.
  They are now relaxed `std::atomic` counters (lock-free).
- **Barrier tracking is O(barriers)** instead of O(barriers × descriptors):
  resource states live in a dedicated `resource → state` index that the
  candidate-acquire path looks up on demand.
- The periodic scout log line gates on a lock-free counter before doing any
  work.

## Configuration

- **New `fsrinj.ini` support** (`src/core/settings.{h,cpp}`, wired through
  `dllmain.cpp` and `overlay_dx12.cpp`). Place the file next to `dxgi.dll`;
  every existing `FSRINJ_*` environment variable works as an INI key with the
  same name, and env vars still take priority. See `fsrinj.ini.example`.
- **New keys:** `FSRINJ_KEY_MENU` (rebind the menu toggle, hex VK codes
  accepted) and `FSRINJ_SHARPNESS` (shared default sharpness).

## Input

- **Hotkeys use proper edge detection and a focus gate**
  (`overlay_dx12.cpp`). The unreliable `GetAsyncKeyState` 0x0001 bit is gone;
  edges are tracked on 0x8000, and hotkeys only fire while the game window is
  foreground — typing in another app can no longer toggle injector features.
  Edge state still updates in the background so refocusing never replays a key.

## Logging

- **Wide-character log paths** (`src/core/log.cpp`): non-ASCII install paths
  (previously mangled by the `CP_ACP` conversion) now work.
- **Writable fallback:** if the game folder can't be written (Program Files),
  the log goes to `%LOCALAPPDATA%\fsr-injector\<game>.log` instead of silently
  disappearing.

## Build / repo

- **`LICENSE`** added (MIT).
- **`.gitignore`** added.
- **MinHook pinned to `v1.3.4`** in `CMakeLists.txt` for reproducible builds
  (same reasoning as the existing ImGui v1.90.9 pin).
- **MSVC Release builds now emit `dxgi.pdb`** (`/Zi` + `/DEBUG /OPT:REF
  /OPT:ICF`) so user crash dumps can be symbolized; the CI artifact includes it
  along with `fsrinj.ini.example`.
- **New `release.yml` workflow:** pushing a tag like `v0.3.0` publishes a
  GitHub Release zip (DLL + PDB + example INI + README + LICENSE).
- **README:** prominent anti-cheat/ban warning, configuration section (INI +
  env), log-location note, waitable-swapchain note, local-build instructions,
  license link.

## Deliberately NOT changed (needs in-game testing / larger refactors)

- Splitting `overlay_dx12.cpp` into modules mirroring the DX11 layout.
- Replacing the EASU/RCAS-inspired shaders with AMD's actual MIT-licensed
  FidelityFX FSR1 headers, and adopting the FidelityFX Optical Flow component.
- History ping-pong to remove one full-res copy per frame; dedicated present
  thread for pacing.
- HDR (scRGB / HDR10) colorspace handling for the sharpen pass.
