#pragma once

// Installs vtable hooks on IDXGISwapChain::Present and ::ResizeBuffers using
// MinHook. The Present address is obtained by spinning up a throwaway device +
// swapchain at init (the "Kiero" technique) -- the vtable is shared per type, so
// hooking it catches the game's real swapchain too.
namespace hooks {

bool install_swapchain_hooks();
void remove_swapchain_hooks();

} // namespace hooks
