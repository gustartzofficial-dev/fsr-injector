#pragma once
struct ID3D11ShaderResourceView;

// ReShade-style "generic depth": hooks OMSetRenderTargets, watches which depth-
// stencil buffer the game binds, and heuristically picks the main scene depth
// (screen-ish aspect ratio, largest). Exposes it as an SRV for frame gen.
namespace depth {

bool install();      // call AFTER swapchain hooks (shares the MinHook session)
void shutdown();

ID3D11ShaderResourceView* current_srv();   // null until a readable depth is found

// diagnostics for the overlay/log
bool        found();
unsigned    width();
unsigned    height();
bool        readable();      // true if we could make an SRV (game allowed it)
const char* fmt_name();

} // namespace depth
