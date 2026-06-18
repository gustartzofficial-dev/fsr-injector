#pragma once
struct ID3D11Device;
struct ID3D11Texture2D;

// ============================================================================
// FSR integration boundary.
//
// This header is the seam between the (working) injection harness and the FSR
// engine. The next phase fills these in against the FidelityFX SDK:
//
//   * Upscaling (D3D11): ffxFsr... context on a DX11 backend. Needs the game's
//     color/motion/depth. In the HIJACK path we get these from the upscaler call
//     we intercept (DLSS/XeSS/FSR2). In the BARE path we only have the backbuffer,
//     so upscaling degrades to spatial (EASU-only / no motion vectors).
//
//   * Frame generation: FSR FG is DX12-native. On a D3D11 game this requires a
//     parallel D3D12 device, shared-handle interop for the frames, FG dispatch on
//     D3D12, and presentation through a D3D12 proxy swapchain. Scheduled last.
//
// Everything below is intentionally a stub that reports "not built" so the rest
// of the app runs and the overlay is testable today.
// ============================================================================

namespace fsr {

struct UpscaleInputs {
    ID3D11Texture2D* color  = nullptr;  // render-res color
    ID3D11Texture2D* depth  = nullptr;  // optional (hijack path)
    ID3D11Texture2D* motion = nullptr;  // optional (hijack path)
    ID3D11Texture2D* output = nullptr;  // display-res target
    float jitter_x = 0.f, jitter_y = 0.f;
    float sharpness = 0.5f;
    int   quality_mode = 0;
};

bool init(ID3D11Device* dev);              // create FSR context(s)
bool upscale(const UpscaleInputs& in);     // run a frame; false if not built/failed
void shutdown();

const char* status_string();               // shown in the overlay

} // namespace fsr
