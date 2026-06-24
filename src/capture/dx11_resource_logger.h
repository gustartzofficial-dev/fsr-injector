#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace capture::dx11log {

bool install();
void shutdown();
void note_swapchain_present(IDXGISwapChain* sc);
void note_omset_render_targets(UINT num_rtvs, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);

}
