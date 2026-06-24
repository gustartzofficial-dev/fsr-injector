# DXVK / Vulkan Migration Plan

## Why DXVK

Sonic Frontiers is still a D3D11 title, but DXVK translates the game into Vulkan. This gives us a cleaner analysis path in Nsight Graphics and can become the final required runtime path if the Vulkan implementation proves easier than native D3D11 resource extraction.

## Final target architecture

```text
SonicFrontiers.exe
  -> DXVK d3d11.dll
  -> DXVK dxgi.dll
  -> Vulkan
  -> future fsr-frontiers Vulkan layer / hook
```

The current `dxgi.dll` proxy cannot be used at the same time as DXVK's `dxgi.dll` unless a proxy chain is deliberately implemented. For analysis, use DXVK only.

## Implementation phases

### Phase 1: Analysis only

Use DXVK + Nsight Graphics. Identify:

- HDR/scene color
- velocity buffer
- depth/stencil
- TAA/final resolve pass
- pre-UI color target
- swapchain format/resolution

### Phase 2: Vulkan layer skeleton

Add a Vulkan path separate from the current D3D11 proxy:

```text
src/vulkan/
  vk_layer_entry.cpp
  vk_dispatch_table.cpp
  vk_swapchain_hook.cpp
  vk_resource_tracker.cpp
  vk_fsr3_bridge.cpp
layer_manifest/
  fsr_frontiers_layer.json
```

### Phase 3: Resource tracker

Track:

- image creation format/extent/usage
- render pass/color attachments
- descriptor set image bindings
- pipeline hash
- draw/dispatch order

### Phase 4: FSR integration

Once the profile is confirmed, feed:

- color input
- depth
- motion vectors after decode
- jitter if found
- pre-UI output
- optional reactive/stencil mask

into a Vulkan FSR path.

## Current code policy

Do not remove the existing DXGI/D3D11 path until the Vulkan path can launch, capture, and present safely.
