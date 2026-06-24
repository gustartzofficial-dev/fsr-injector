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

    unsigned dx12_command_lists_seen = 0;
    unsigned dx12_execute_calls = 0;
    unsigned dx12_draw_calls = 0;
    unsigned dx12_resource_barriers = 0;
    unsigned dx12_rtv_descriptors = 0;
    unsigned dx12_dsv_descriptors = 0;
    unsigned dx12_om_depth_binds = 0;
    unsigned dx12_om_rt_binds = 0;
    unsigned dx12_pso_sets = 0;
    unsigned dx12_root_table_sets = 0;

    unsigned dx12_depth_candidates = 0;
    unsigned dx12_motion_candidates = 0;
    unsigned dx12_best_depth_width = 0;
    unsigned dx12_best_depth_height = 0;
    DXGI_FORMAT dx12_best_depth_format = DXGI_FORMAT_UNKNOWN;
    unsigned dx12_best_motion_width = 0;
    unsigned dx12_best_motion_height = 0;
    DXGI_FORMAT dx12_best_motion_format = DXGI_FORMAT_UNKNOWN;
    bool dx12_best_motion_resource_available = false;
};

void set_enabled(bool enabled);

// While set on the current thread, the scout ignores command-list/queue activity
// so the injector's own overlay + ImGui draws are not mistaken for game rendering
// and ImGui's internal font-upload list/queue is not detoured by our hooks.
void set_overlay_active(bool active);

void note_dx12_swapchain(unsigned width, unsigned height, DXGI_FORMAT format);
void note_dx12_history(bool ready);
void note_final_frame_motion(bool available);

void note_dx12_command_list_seen();
void note_dx12_execute_call(unsigned command_list_count);
void note_dx12_draw_call(bool indexed);
void note_dx12_resource_barrier(unsigned count, const D3D12_RESOURCE_BARRIER* barriers);
void note_dx12_set_pipeline_state();
void note_dx12_set_graphics_root_descriptor_table();
void note_dx12_rtv_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc);
void note_dx12_dsv_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc);
void note_dx12_omset(unsigned rt_count, const D3D12_CPU_DESCRIPTOR_HANDLE* rt_handles, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv_handle);

// Returns AddRef'd resource for the current best motion/velocity-like candidate.
// Caller must Release(). This is experimental and may be a false positive.
bool acquire_dx12_best_motion_candidate(ID3D12Resource** out_resource, DXGI_FORMAT* out_format, unsigned* out_width, unsigned* out_height, D3D12_RESOURCE_STATES* out_state, bool* out_state_known);

Snapshot snapshot();
void log_snapshot_once();
void log_dx12_candidates_periodic();

} // namespace capture::scout
