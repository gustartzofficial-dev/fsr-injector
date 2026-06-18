#include "proxy/dxgi_proxy.h"
#include "core/log.h"

namespace proxy {

FARPROC p_CreateDXGIFactory               = nullptr;
FARPROC p_CreateDXGIFactory1              = nullptr;
FARPROC p_CreateDXGIFactory2             = nullptr;
FARPROC p_DXGIGetDebugInterface1          = nullptr;
FARPROC p_DXGIDeclareAdapterRemovalSupport= nullptr;

static HMODULE g_real = nullptr;

bool load_real_dxgi() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring path = std::wstring(sys) + L"\\dxgi.dll";
    g_real = LoadLibraryW(path.c_str());
    if (!g_real) {
        LOGF("[proxy] FAILED to load real dxgi.dll");
        return false;
    }
    p_CreateDXGIFactory                = GetProcAddress(g_real, "CreateDXGIFactory");
    p_CreateDXGIFactory1               = GetProcAddress(g_real, "CreateDXGIFactory1");
    p_CreateDXGIFactory2               = GetProcAddress(g_real, "CreateDXGIFactory2");
    p_DXGIGetDebugInterface1           = GetProcAddress(g_real, "DXGIGetDebugInterface1");
    p_DXGIDeclareAdapterRemovalSupport = GetProcAddress(g_real, "DXGIDeclareAdapterRemovalSupport");
    LOGF("[proxy] real dxgi.dll loaded @ %p", (void*)g_real);
    return true;
}

void unload_real_dxgi() {
    if (g_real) { FreeLibrary(g_real); g_real = nullptr; }
}

} // namespace proxy
