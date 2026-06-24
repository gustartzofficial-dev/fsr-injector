# Sonic Frontiers DXVK FSR Profile

This project is being tailored for Sonic Frontiers. The current stable runtime is still the original DXGI/D3D11 proxy path, but DXVK is now the preferred analysis path because Nsight Graphics can inspect the translated Vulkan frame.

## Current candidate findings

| Resource | Candidate | Status |
|---|---:|---|
| Present | `vkQueuePresentKHR` around event `31220` | confirmed in capture |
| Swapchain | `1920x1080` | confirmed in capture |
| Motion vectors | `Image_17361`, `R8G8B8A8_UNORM`, `2560x1440`, first seen around event `13120` | strong candidate, not final |
| Motion decode | `(raw.rg - 0.5) * 2.0 * scale` | hypothesis |
| Depth | `D32_FLOAT` or `D32_FLOAT_S8X24_UINT` | to confirm |
| HUDless color | pre-UI color target, likely UNORM or HDR source before final UI | to confirm |

## What still needs verification

1. Confirm that `Image_17361` is read by the TAA/final resolve pass.
2. Compare still-camera and fast-camera captures. A real velocity buffer should change strongly.
3. Verify R-only and G-only views. R should mostly encode horizontal motion and G vertical motion.
4. Identify the depth image and confirm reverse-Z behavior.
5. Identify the pre-UI output target.

## Important rule

Do not hook by event number. Event numbers change. Hook by pipeline/shader hash, image format, image extent, and the simultaneous resource binding pattern.
