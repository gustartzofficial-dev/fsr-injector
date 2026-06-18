#include "fsr/fsr_integration.h"
#include "core/log.h"

// STUB. Replaced in the FidelityFX-integration phase. Kept compiling and honest
// so the harness/overlay are runnable now.
namespace fsr {

bool init(ID3D11Device*) {
    LOGF("[fsr] init() called -- engine not built yet (stub)");
    return false;
}

bool upscale(const UpscaleInputs&) {
    return false; // no-op until FidelityFX SDK is wired in
}

void shutdown() {}

const char* status_string() { return "not built (stub)"; }

} // namespace fsr
