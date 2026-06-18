#pragma once
#include <windows.h>

// Loads the real System32\dxgi.dll and resolves the exports we forward.
// We forward the common factory/debug entry points used by D3D11/D3D12 games.
// Interception of frames does NOT happen here -- it happens via the Present
// vtable hook in hooks/. This proxy only guarantees early load + transparency.
namespace proxy {

bool load_real_dxgi();   // returns false if System32\dxgi.dll can't be loaded
void unload_real_dxgi();

// Resolved function pointers from the real dxgi.dll.
extern FARPROC p_CreateDXGIFactory;
extern FARPROC p_CreateDXGIFactory1;
extern FARPROC p_CreateDXGIFactory2;
extern FARPROC p_DXGIGetDebugInterface1;
extern FARPROC p_DXGIDeclareAdapterRemovalSupport;

} // namespace proxy
