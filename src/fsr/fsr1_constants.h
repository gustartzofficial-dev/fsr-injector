#pragma once
#include <cstdint>

// CPU-side constant packing for AMD FidelityFX Super Resolution 1.0 (the real,
// MIT-licensed ffx_fsr1.h vendored under third_party/ffx). These 32-bit blocks
// are pushed to the shaders as root constants.
namespace fsr1 {

struct EasuConstants { uint32_t con[16]; }; // con0..con3, 4 uints each
struct RcasConstants { uint32_t con[4]; };

// input:  resolution the image was rendered at (the low-res texture)
// output: resolution being upscaled to (the swapchain)
void easu_constants(EasuConstants& out,
                    float input_width, float input_height,
                    float output_width, float output_height);

// sharpness_stops: 0.0 = maximum sharpening, each +1.0 halves it (AMD's scale).
void rcas_constants(RcasConstants& out, float sharpness_stops);

// Map the injector's 0..1 sharpness slider (1 = sharpest) onto RCAS stops.
float slider_to_stops(float slider01);

} // namespace fsr1
