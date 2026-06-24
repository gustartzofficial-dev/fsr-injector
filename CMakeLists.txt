cmake_minimum_required(VERSION 3.26)
project(fsr_injector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows / DXGI only.")
endif()

include(FetchContent)

# --- MinHook (vtable / API hooking) ---
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# --- Dear ImGui (overlay) ---
# Pinned to v1.90.9: the last line of releases using the classic ImGui_ImplDX12_Init
# signature. 'master' switched DX12 to a new InitInfo API, which is what was crashing
# the DX12 overlay (API/ABI mismatch). The win32 + dx11 backends are unaffected.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
)
FetchContent_MakeAvailable(imgui)

# ImGui has no CMake of its own; compile the core + the win32/dx11 backends.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

# MinGW's d3d12 headers miss PFN_D3D12_SERIALIZE_ROOT_SIGNATURE; force-include a shim
# providing it, scoped to just the DX12 backend translation unit.
set_source_files_properties(
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/cmake/mingw_d3d12_shim.h"
)

# --- Our proxy DLL ---
# Target is named "fsr_proxy" to avoid colliding with the system "dxgi" import
# library it links against; the OUTPUT_NAME below makes the file "dxgi.dll".
add_library(fsr_proxy SHARED
    src/dllmain.cpp
    src/proxy/dxgi_proxy.cpp
    src/hooks/swapchain_hook.cpp
    src/hooks/depth_hook.cpp
    src/hooks/dx12_queue_capture.cpp
    src/capture/generic_resource_scout.cpp
    src/overlay/overlay.cpp
    src/overlay/overlay_dx12.cpp
    src/detect/upscaler_detect.cpp
    src/fsr/fsr_integration.cpp
    src/fsr/framegen.cpp
    src/fsr/upscaler.cpp
    src/core/log.cpp
)

target_include_directories(fsr_proxy PRIVATE src)
target_compile_definitions(fsr_proxy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(fsr_proxy PRIVATE minhook imgui d3d11 d3d12 dxgi dxguid dwmapi d3dcompiler)

# Name the output exactly "dxgi.dll" with no "lib" prefix.
set_target_properties(fsr_proxy PROPERTIES PREFIX "" OUTPUT_NAME "dxgi")
