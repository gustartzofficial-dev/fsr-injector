#include "capture/generic_resource_scout.h"
#include "hooks/depth_hook.h"
#include "core/log.h"

#include <mutex>

namespace capture::scout {
namespace {
    std::mutex g_mtx;
    Snapshot g_state{};
    bool g_logged = false;
}

void set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.enabled = enabled;
}

void note_dx12_swapchain(unsigned width, unsigned height, DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.api = ApiKind::DX12;
    g_state.dx12_width = width;
    g_state.dx12_height = height;
    g_state.dx12_format = format;
}

void note_dx12_history(bool ready) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.dx12_history_ready = ready;
}

void note_final_frame_motion(bool available) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.final_frame_motion = available;
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lk(g_mtx);
    Snapshot out = g_state;
    out.dx11_depth_found = depth::found();
    out.dx11_depth_readable = depth::readable();
    out.dx11_depth_width = depth::width();
    out.dx11_depth_height = depth::height();
    out.dx11_depth_format = depth::fmt_name();
    return out;
}

void log_snapshot_once() {
    Snapshot s = snapshot();
    if (g_logged) return;
    g_logged = true;
    LOGF("[scout] generic resource scout active: api=%s dx12=%ux%u fmt=%u motion=%s dx11_depth=%s %ux%u fmt=%s readable=%s",
         s.api == ApiKind::DX12 ? "dx12" : (s.api == ApiKind::DX11 ? "dx11" : "unknown"),
         s.dx12_width, s.dx12_height, (unsigned)s.dx12_format,
         s.final_frame_motion ? "final-frame-oflow" : "none",
         s.dx11_depth_found ? "found" : "none",
         s.dx11_depth_width, s.dx11_depth_height, s.dx11_depth_format ? s.dx11_depth_format : "none",
         s.dx11_depth_readable ? "yes" : "no");
}

} // namespace capture::scout
