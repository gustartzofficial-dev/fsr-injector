#pragma once
struct IDXGISwapChain;

// Custom adaptive sharpener applied to the final frame. RCAS-inspired contrast-
// adaptive sharpening, upgraded with a noise floor (skip flat/noisy areas) and a
// deringing clamp (no overshoot halos -- a known vanilla-FSR1 weakness).
// Driven by config: upscaling_enabled + sharpness.
namespace upscaler {

void sharpen(IDXGISwapChain* sc);   // call once per real frame, before the overlay
void on_resize();
void shutdown();

} // namespace upscaler
