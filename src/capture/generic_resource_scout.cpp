#include "capture/generic_resource_scout.h"
#include "hooks/depth_hook.h"
#include "core/log.h"

#include <atomic>
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
        uint64_t seq = 0;                       // insertion order, for eviction
        // STRONG reference (AddRef'd on store, Release'd on overwrite/evict/reset).
        // The previous raw pointer was a use-after-free: games destroy and recreate
        // render targets constantly, and AddRef on a freed COM pointer is itself
        // the crash. Holding a real ref trades a bounded amount of VRAM lifetime
        // extension (capped by kMaxDescriptors + the ResizeBuffers reset) for
        // guaranteed pointer validity.
        ID3D12Resource* resource = nullptr;
    };

    // ---- hot-path counters -------------------------------------------------
    // These are bumped from inside DrawInstanced / SetPipelineState / etc. hooks,
    // i.e. tens of thousands of times per frame from multiple render threads.
    // They MUST NOT take a lock: a shared mutex here serializes the game's entire
    // command-recording thread pool and measurably costs FPS. Relaxed atomics are
    // exact enough for diagnostics and effectively free.
    std::atomic<unsigned> g_command_lists_seen{0};
    std::atomic<unsigned> g_execute_calls{0};
    std::atomic<unsigned> g_draw_calls{0};
    std::atomic<unsigned> g_resource_barriers{0};
    std::atomic<unsigned> g_pso_sets{0};
    std::atomic<unsigned> g_root_table_sets{0};
    std::atomic<unsigned> g_rtv_descriptors{0};
    std::atomic<unsigned> g_dsv_descriptors{0};
    std::atomic<unsigned> g_om_rt_binds{0};
    std::atomic<unsigned> g_om_depth_binds{0};
    std::atomic<unsigned> g_depth_candidates{0};
    std::atomic<unsigned> g_motion_candidates{0};

    // ---- cold state (mutex-protected) --------------------------------------
    std::mutex g_mtx;
    Snapshot g_state{};                 // only the non-counter fields are used
    bool g_logged = false;
    std::atomic<unsigned> g_periodic_log_count{0};
    uint64_t g_next_seq = 1;
    std::unordered_map<SIZE_T, DescriptorInfo> g_descriptors;
    // Secondary index: last known state per resource, updated by the barrier
    // hook in O(barrier count) instead of the old O(barriers x descriptors)
    // scan (which also ran under the lock on a hot path).
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> g_resource_states;

    constexpr size_t kMaxDescriptors = 4096;
    constexpr size_t kMaxResourceStates = 8192;

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

    void release_descriptor_locked(DescriptorInfo& info) {
        if (info.resource) { info.resource->Release(); info.resource = nullptr; }
    }

    // Insert/overwrite a descriptor entry, managing the resource references and
    // keeping the map bounded. Caller passes info with resource ALREADY AddRef'd.
    void store_descriptor_locked(SIZE_T handle, DescriptorInfo&& info) {
        auto it = g_descriptors.find(handle);
        if (it != g_descriptors.end()) {
            release_descriptor_locked(it->second);   // descriptor slot reused: drop old ref
            it->second = std::move(info);
            return;
        }
        if (g_descriptors.size() >= kMaxDescriptors) {
            // Evict the oldest non-candidate entry (or the overall oldest if all
            // are candidates) so tracking can't grow without bound in games that
            // churn descriptor heaps.
            auto victim = g_descriptors.end();
            uint64_t best_seq = UINT64_MAX;
            for (auto v = g_descriptors.begin(); v != g_descriptors.end(); ++v) {
                const bool cand = v->second.depth_candidate || v->second.motion_candidate;
                const uint64_t score = v->second.seq + (cand ? (UINT64_MAX / 2) : 0);
                if (score < best_seq) { best_seq = score; victim = v; }
            }
            if (victim != g_descriptors.end()) {
                release_descriptor_locked(victim->second);
                g_descriptors.erase(victim);
            }
        }
        g_descriptors.emplace(handle, std::move(info));
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

// Hot path: lock-free.
void note_dx12_command_list_seen() { g_command_lists_seen.fetch_add(1, std::memory_order_relaxed); }
void note_dx12_execute_call(unsigned command_list_count) {
    if (g_overlay_active) return;
    g_execute_calls.fetch_add(1, std::memory_order_relaxed);
    g_command_lists_seen.fetch_add(command_list_count, std::memory_order_relaxed);
}
void note_dx12_draw_call(bool) { if (g_overlay_active) return; g_draw_calls.fetch_add(1, std::memory_order_relaxed); }
void note_dx12_set_pipeline_state() { if (g_overlay_active) return; g_pso_sets.fetch_add(1, std::memory_order_relaxed); }
void note_dx12_set_graphics_root_descriptor_table() { if (g_overlay_active) return; g_root_table_sets.fetch_add(1, std::memory_order_relaxed); }

void note_dx12_resource_barrier(unsigned count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (g_overlay_active) return;
    g_resource_barriers.fetch_add(count, std::memory_order_relaxed);
    if (!barriers) return;
    // Only transition barriers on resources matter; the map update is O(count).
    std::lock_guard<std::mutex> lk(g_mtx);
    for (unsigned i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
        if (g_resource_states.size() >= kMaxResourceStates) g_resource_states.clear(); // stale keys are harmless; bound memory
        g_resource_states[b.Transition.pResource] = b.Transition.StateAfter;
    }
}

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
    resource->AddRef();
    info.resource = resource;

    std::lock_guard<std::mutex> lk(g_mtx);
    info.seq = g_next_seq++;
    info.motion_candidate = near_swap_size(info.width, info.height) && is_motion_like_format(info.view_format != DXGI_FORMAT_UNKNOWN ? info.view_format : info.resource_format);
    g_rtv_descriptors.fetch_add(1, std::memory_order_relaxed);
    if (info.motion_candidate) g_motion_candidates.fetch_add(1, std::memory_order_relaxed);
    update_best_locked(info);
    store_descriptor_locked(handle.ptr, std::move(info));
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
    resource->AddRef();
    info.resource = resource;

    std::lock_guard<std::mutex> lk(g_mtx);
    info.seq = g_next_seq++;
    info.depth_candidate = near_swap_size(info.width, info.height) && (is_depth_format(info.view_format) || is_depth_format(info.resource_format));
    g_dsv_descriptors.fetch_add(1, std::memory_order_relaxed);
    if (info.depth_candidate) g_depth_candidates.fetch_add(1, std::memory_order_relaxed);
    update_best_locked(info);
    store_descriptor_locked(handle.ptr, std::move(info));
}

void note_dx12_omset(unsigned rt_count, const D3D12_CPU_DESCRIPTOR_HANDLE* rt_handles, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv_handle) {
    if (g_overlay_active) return;
    g_om_rt_binds.fetch_add(rt_count, std::memory_order_relaxed);
    if (dsv_handle && dsv_handle->ptr) g_om_depth_binds.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (dsv_handle && dsv_handle->ptr) {
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
    const DescriptorInfo* best = nullptr;
    for (const auto& kv : g_descriptors) {
        const DescriptorInfo& info = kv.second;
        if (!info.motion_candidate || !info.resource) continue;
        if (!best || (info.bind_count > best->bind_count) ||
            (info.bind_count == best->bind_count && info.width * info.height >= best->width * best->height)) {
            best = &info;
        }
    }
    if (!best) return false;
    // The map holds a strong ref, so this AddRef is on a guaranteed-live object
    // (the whole point of the strong-ref change).
    best->resource->AddRef();
    if (out_resource) *out_resource = best->resource;
    else best->resource->Release();
    if (out_format) *out_format = best->view_format != DXGI_FORMAT_UNKNOWN ? best->view_format : best->resource_format;
    if (out_width) *out_width = best->width;
    if (out_height) *out_height = best->height;
    auto st = g_resource_states.find(best->resource);
    if (st != g_resource_states.end()) {
        if (out_state) *out_state = st->second;
        if (out_state_known) *out_state_known = true;
    }
    return true;
}

void reset_dx12_resources() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& kv : g_descriptors) release_descriptor_locked(kv.second);
    g_descriptors.clear();
    g_resource_states.clear();
    g_state.dx12_best_depth_width = 0;
    g_state.dx12_best_depth_height = 0;
    g_state.dx12_best_depth_format = DXGI_FORMAT_UNKNOWN;
    g_state.dx12_best_motion_width = 0;
    g_state.dx12_best_motion_height = 0;
    g_state.dx12_best_motion_format = DXGI_FORMAT_UNKNOWN;
    g_state.dx12_best_motion_resource_available = false;
    LOGF("[scout] dx12 resource cache reset");
}

Snapshot snapshot() {
    Snapshot out;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        out = g_state;
    }
    out.dx12_command_lists_seen = g_command_lists_seen.load(std::memory_order_relaxed);
    out.dx12_execute_calls = g_execute_calls.load(std::memory_order_relaxed);
    out.dx12_draw_calls = g_draw_calls.load(std::memory_order_relaxed);
    out.dx12_resource_barriers = g_resource_barriers.load(std::memory_order_relaxed);
    out.dx12_pso_sets = g_pso_sets.load(std::memory_order_relaxed);
    out.dx12_root_table_sets = g_root_table_sets.load(std::memory_order_relaxed);
    out.dx12_rtv_descriptors = g_rtv_descriptors.load(std::memory_order_relaxed);
    out.dx12_dsv_descriptors = g_dsv_descriptors.load(std::memory_order_relaxed);
    out.dx12_om_rt_binds = g_om_rt_binds.load(std::memory_order_relaxed);
    out.dx12_om_depth_binds = g_om_depth_binds.load(std::memory_order_relaxed);
    out.dx12_depth_candidates = g_depth_candidates.load(std::memory_order_relaxed);
    out.dx12_motion_candidates = g_motion_candidates.load(std::memory_order_relaxed);
    out.dx11_depth_found = depth::found();
    out.dx11_depth_readable = depth::readable();
    out.dx11_depth_width = depth::width();
    out.dx11_depth_height = depth::height();
    out.dx11_depth_format = depth::fmt_name();
    return out;
}

void log_snapshot_once() {
    Snapshot s = snapshot();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_logged) return;
        g_logged = true;
    }
    LOGF("[scout] generic resource scout active: api=%s dx12=%ux%u fmt=%u motion=%s dx11_depth=%s %ux%u fmt=%s readable=%s",
         s.api == ApiKind::DX12 ? "dx12" : (s.api == ApiKind::DX11 ? "dx11" : "unknown"),
         s.dx12_width, s.dx12_height, (unsigned)s.dx12_format,
         s.final_frame_motion ? "final-frame-oflow" : "none",
         s.dx11_depth_found ? "found" : "none",
         s.dx11_depth_width, s.dx11_depth_height, s.dx11_depth_format ? s.dx11_depth_format : "none",
         s.dx11_depth_readable ? "yes" : "no");
}

void log_dx12_candidates_periodic() {
    // Lock-free counter gate first: this is called from ExecuteCommandLists,
    // so the common case (not this tick) must stay cheap.
    if ((g_periodic_log_count.fetch_add(1, std::memory_order_relaxed) + 1) % 600 != 0) return;
    Snapshot s = snapshot();
    LOGF("[scout-dx12] cmdlists=%u exec=%u draws=%u barriers=%u pso=%u rootTbl=%u rtv=%u dsv=%u omrt=%u omdsv=%u depthCand=%u bestDepth=%ux%u %s mvCand=%u bestMV=%ux%u %s",
         s.dx12_command_lists_seen, s.dx12_execute_calls, s.dx12_draw_calls, s.dx12_resource_barriers, s.dx12_pso_sets, s.dx12_root_table_sets,
         s.dx12_rtv_descriptors, s.dx12_dsv_descriptors, s.dx12_om_rt_binds, s.dx12_om_depth_binds,
         s.dx12_depth_candidates, s.dx12_best_depth_width, s.dx12_best_depth_height, fmt_name(s.dx12_best_depth_format),
         s.dx12_motion_candidates, s.dx12_best_motion_width, s.dx12_best_motion_height, fmt_name(s.dx12_best_motion_format));
}

} // namespace capture::scout
