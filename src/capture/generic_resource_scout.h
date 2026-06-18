#pragma once

#include <d3d12.h>
#include <dxgi.h>

namespace capture::scout {

enum class ApiKind { Unknown, DX11, DX12 };

struct Snapshot {
    bool enabled = true;
    ApiKind api = ApiKind::Unknown;
    bool final_frame_motion = false;
    bool dx11_depth_found = false;
    bool dx11_depth_readable = false;
    unsigned dx11_depth_width = 0;
    unsigned dx11_depth_height = 0;
    const char* dx11_depth_format = "none";
    unsigned dx12_width = 0;
    unsigned dx12_height = 0;
    DXGI_FORMAT dx12_format = DXGI_FORMAT_UNKNOWN;
    bool dx12_history_ready = false;
};

void set_enabled(bool enabled);
void note_dx12_swapchain(unsigned width, unsigned height, DXGI_FORMAT format);
void note_dx12_history(bool ready);
void note_final_frame_motion(bool available);
Snapshot snapshot();
void log_snapshot_once();

} // namespace capture::scout
