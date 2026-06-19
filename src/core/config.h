#pragma once
#include <atomic>

// Live state shared between the overlay (writer) and the frame path (reader).
// Kept as atomics so it can be read from the present thread without locks.
namespace core {

enum class UpscalerMode { Off, FSR_Quality, FSR_Balanced, FSR_Performance, FSR_UltraPerf };

struct Config {
    std::atomic<bool>  overlay_visible{true};
    std::atomic<bool>  upscaling_enabled{false};
    std::atomic<int>   upscaler_mode{(int)UpscalerMode::FSR_Quality};
    std::atomic<float> sharpness{0.5f};

    std::atomic<bool>  framegen_enabled{false};
    std::atomic<int>   framegen_multiplier{2}; // 2 = interpolate one frame
    std::atomic<bool>  use_depth{false}; // depth-assisted disocclusion (default off)
    std::atomic<bool>  dx11_frame_pacing{true}; // Present generated frames on the midpoint path
    std::atomic<bool>  dx11_overlay_in_generated{true}; // draw overlay before DX11 FG capture to avoid flicker

    std::atomic<int>   toggle_key{0x24}; // VK_HOME
};

Config& config();

} // namespace core
