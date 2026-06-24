#include "capture/generic_resource_scout.h"
#include "hooks/depth_hook.h"
#include "core/log.h"

#include <mutex>
#include <unordered_map>
#include <algorithm>

namespace capture::scout {
namespace {
    struct DescriptorInfo {
        bool valid = false;
        bool dsv = false;
        bool rtv = false;
        unsigned width = 0;
        unsigned height = 0;
        DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
        bool depth_candidate = false;
        bool motion_candidate = false;
        unsigned bind_count = 0;
        ID3D12Resource* resource = nullptr; // raw pointer; AddRef only during acquire.
        bool state_known = false;
        D3D12_RESOURCE_STATES last_state = D3D12_RESOURCE_STATE_COMMON;
    };

    std::mutex g_mtx;
    Snapshot g_state{};
    bool g_logged = false;
    unsigned g_periodic_log_count = 0;
    std::unordered_map<SIZE_T, DescriptorInfo> g_descriptors;

    // Set only while the injector is recording its own overlay/ImGui commands on
    // this thread. Lets the hooks below skip our own work (and ImGui's internal
    // font-upload list/queue) so it is neither profiled as game rendering nor
    // re-entered by the generic command-list hooks.
    thread_local bool g_overlay_active = false;

    bool is_depth_format(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R16_TYPELESS:
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_R32_TYPELESS:
            case DXGI_FORMAT_R32G8X24_TYPELESS:
                return true;
            default:
                return false;
        }
    }

    bool is_motion_like_format(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R32G32_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return true;
            default:
                return false;
        }
    }

    bool near_swap_size(unsigned w, unsigned h) {
        if (!g_state.dx12_width || !g_state.dx12_height || !w || !h) return false;
        const unsigned min_w = g_state.dx12_width / 2;
        const unsigned min_h = g_state.dx12_height / 2;
        const unsigned max_w = g_state.dx12_width * 2;
        const unsigned max_h = g_state.dx12_height * 2;
        return w >= min_w && h >= min_h && w <= max_w && h <= max_h;
    }

    const char* fmt_name(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_UNKNOWN: return "unknown";
            case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
            case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
            case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
            case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
            case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
            case DXGI_FORMAT_D16_UNORM: return "D16";
            case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
            case DXGI_FORMAT_D32_FLOAT: return "D32F";
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32FS8";
            case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
            case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
            case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
            case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
            default: return "fmt";
        }
    }

    void update_best_locked(const DescriptorInfo& info) {
        if (info.depth_candidate) {
            if (!g_state.dx12_best_depth_width || (info.width * info.height >= g_state.dx12_best_depth_width * g_state.dx12_best_depth_height)) {
                g_state.dx12_best_depth_width = info.width;
                g_state.dx12_best_depth_height = info.height;
                g_state.dx12_best_depth_format = info.view_format != DXGI_FORMAT_UNKNOWN ? info.view_format : info.resource_format;
            }
        }
        if (info.motion_candidate) {
            if (!g_state.dx12_best_motion_width || (info.width * info.height >= g_state.dx12_best_motion_width * g_state.dx12_best_motion_height)) {
                g_state.dx12_best_motion_width = info.width;
                g_state.dx12_best_motion_height = info.height;
                g_state.dx12_best_motion_format = info.view_format != DXGI_FORMAT_UNKNOWN ? info.view_format : info.resource_format;
                g_state.dx12_best_motion_resource_available = info.resource != nullptr;
            }
        }
    }
}

void set_enabled(bool enabled) { std::lock_guard<std::mutex> lk(g_mtx); g_state.enabled = enabled; }

void set_overlay_active(bool active) { g_overlay_active = active; }

void note_dx12_swapchain(unsigned width, unsigned height, DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.api = ApiKind::DX12;
    g_state.dx12_width = width;
    g_state.dx12_height = height;
    g_state.dx12_format = format;
}

void note_dx12_history(bool ready) { std::lock_guard<std::mutex> lk(g_mtx); g_state.dx12_history_ready = ready; }
void note_final_frame_motion(bool available) { std::lock_guard<std::mutex> lk(g_mtx); g_state.final_frame_motion = available; }
void note_dx12_command_list_seen() { std::lock_guard<std::mutex> lk(g_mtx); ++g_state.dx12_command_lists_seen; }
void note_dx12_execute_call(unsigned command_list_count) { if (g_overlay_active) return; std::lock_guard<std::mutex> lk(g_mtx); ++g_state.dx12_execute_calls; g_state.dx12_command_lists_seen += command_list_count; }
void note_dx12_draw_call(bool) { if (g_overlay_active) return; std::lock_guard<std::mutex> lk(g_mtx); ++g_state.dx12_draw_calls; }
void note_dx12_resource_barrier(unsigned count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (g_overlay_active) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.dx12_resource_barriers += count;
    if (!barriers) return;
    for (unsigned i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
        ID3D12Resource* res = b.Transition.pResource;
        for (auto& kv : g_descriptors) {
            DescriptorInfo& info = kv.second;
            if (info.resource == res) {
                info.state_known = true;
                info.last_state = b.Transition.StateAfter;
            }
        }
    }
}
void note_dx12_set_pipeline_state() { if (g_overlay_active) return; std::lock_guard<std::mutex> lk(g_mtx); ++g_state.dx12_pso_sets; }
void note_dx12_set_graphics_root_descriptor_table() { if (g_overlay_active) return; std::lock_guard<std::mutex> lk(g_mtx); ++g_state.dx12_root_table_sets; }

void note_dx12_rtv_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc) {
    if (!resource || !handle.ptr) return;
    D3D12_RESOURCE_DESC rd = resource->GetDesc();
    DescriptorInfo info{};
    info.valid = true;
    info.rtv = true;
    info.width = static_cast<unsigned>(rd.Width);
    info.height = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? static_cast<unsigned>(rd.DepthOrArraySize) : rd.Height;
    info.resource_format = rd.Format;
    info.view_format = desc ? desc->Format : rd.Format;
    info.resource = resource;
    info.motion_candidate = near_swap_size(info.width, info.height) && is_motion_like_format(info.view_format != DXGI_FORMAT_UNKNOWN ? info.view_format : info.resource_format);

    std::lock_guard<std::mutex> lk(g_mtx);
    g_descriptors[handle.ptr] = info;
    ++g_state.dx12_rtv_descriptors;
    if (info.motion_candidate) ++g_state.dx12_motion_candidates;
    update_best_locked(info);
}

void note_dx12_dsv_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc) {
    if (!resource || !handle.ptr) return;
    D3D12_RESOURCE_DESC rd = resource->GetDesc();
    DescriptorInfo info{};
    info.valid = true;
    info.dsv = true;
    info.width = static_cast<unsigned>(rd.Width);
    info.height = rd.Height;
    info.resource_format = rd.Format;
    info.view_format = desc ? desc->Format : rd.Format;
    info.resource = resource;
    info.depth_candidate = near_swap_size(info.width, info.height) && (is_depth_format(info.view_format) || is_depth_format(info.resource_format));

    std::lock_guard<std::mutex> lk(g_mtx);
    g_descriptors[handle.ptr] = info;
    ++g_state.dx12_dsv_descriptors;
    if (info.depth_candidate) ++g_state.dx12_depth_candidates;
    update_best_locked(info);
}

void note_dx12_omset(unsigned rt_count, const D3D12_CPU_DESCRIPTOR_HANDLE* rt_handles, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv_handle) {
    if (g_overlay_active) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.dx12_om_rt_binds += rt_count;
    if (dsv_handle && dsv_handle->ptr) {
        ++g_state.dx12_om_depth_binds;
        auto it = g_descriptors.find(dsv_handle->ptr);
        if (it != g_descriptors.end()) { ++it->second.bind_count; it->second.depth_candidate = true; update_best_locked(it->second); }
    }
    if (rt_handles) {
        for (unsigned i = 0; i < rt_count; ++i) {
            auto it = g_descriptors.find(rt_handles[i].ptr);
            if (it != g_descriptors.end()) { ++it->second.bind_count; if (it->second.motion_candidate) update_best_locked(it->second); }
        }
    }
}

bool acquire_dx12_best_motion_candidate(ID3D12Resource** out_resource, DXGI_FORMAT* out_format, unsigned* out_width, unsigned* out_height, D3D12_RESOURCE_STATES* out_state, bool* out_state_known) {
    if (out_resource) *out_resource = nullptr;
    if (out_format) *out_format = DXGI_FORMAT_UNKNOWN;
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    if (out_state) *out_state = D3D12_RESOURCE_STATE_COMMON;
    if (out_state_known) *out_state_known = false;
    std::lock_guard<std::mutex> lk(g_mtx);
    ID3D12Resource* best = nullptr;
    DescriptorInfo best_info{};
    for (const auto& kv : g_descriptors) {
        const DescriptorInfo& info = kv.second;
        if (!info.motion_candidate || !info.resource) continue;
        if (!best || (info.bind_count > best_info.bind_count) ||
            (info.bind_count == best_info.bind_count && info.width * info.height >= best_info.width * best_info.height)) {
            best = info.resource;
            best_info = info;
        }
    }
    if (!best) return false;
    best->AddRef();
    if (out_resource) *out_resource = best;
    else best->Release();
    if (out_format) *out_format = best_info.view_format != DXGI_FORMAT_UNKNOWN ? best_info.view_format : best_info.resource_format;
    if (out_width) *out_width = best_info.width;
    if (out_height) *out_height = best_info.height;
    if (out_state) *out_state = best_info.last_state;
    if (out_state_known) *out_state_known = best_info.state_known;
    return true;
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

void log_dx12_candidates_periodic() {
    Snapshot s = snapshot();
    if ((++g_periodic_log_count % 600) != 0) return;
    LOGF("[scout-dx12] cmdlists=%u exec=%u draws=%u barriers=%u pso=%u rootTbl=%u rtv=%u dsv=%u omrt=%u omdsv=%u depthCand=%u bestDepth=%ux%u %s mvCand=%u bestMV=%ux%u %s",
         s.dx12_command_lists_seen, s.dx12_execute_calls, s.dx12_draw_calls, s.dx12_resource_barriers, s.dx12_pso_sets, s.dx12_root_table_sets,
         s.dx12_rtv_descriptors, s.dx12_dsv_descriptors, s.dx12_om_rt_binds, s.dx12_om_depth_binds,
         s.dx12_depth_candidates, s.dx12_best_depth_width, s.dx12_best_depth_height, fmt_name(s.dx12_best_depth_format),
         s.dx12_motion_candidates, s.dx12_best_motion_width, s.dx12_best_motion_height, fmt_name(s.dx12_best_motion_format));
}

} // namespace capture::scout
