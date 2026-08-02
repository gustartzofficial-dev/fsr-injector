#pragma once
#include <d3dcommon.h>
#include <cstring>

// Serves the embedded FidelityFX headers (generated from third_party/ffx at
// build time) to the runtime HLSL compiler, so shaders on BOTH the DX11 and
// DX12 paths can '#include "ffx_a.h"' with no files on disk next to the game.
#include "ffx_a_embedded.h"
#include "ffx_fsr1_embedded.h"

namespace fsr1 {

class FfxInclude : public ID3DInclude {
public:
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR file, LPCVOID,
                                   LPCVOID* out_data, UINT* out_size) override {
        if (!file || !out_data || !out_size) return E_FAIL;
        if (std::strcmp(file, "ffx_a.h") == 0) {
            *out_data = g_ffx_a_h; *out_size = g_ffx_a_h_len; return S_OK;
        }
        if (std::strcmp(file, "ffx_fsr1.h") == 0) {
            *out_data = g_ffx_fsr1_h; *out_size = g_ffx_fsr1_h_len; return S_OK;
        }
        return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};

inline FfxInclude& include_handler() {
    static FfxInclude handler;
    return handler;
}

} // namespace fsr1
