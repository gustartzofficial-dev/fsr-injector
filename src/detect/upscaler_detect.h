#pragma once
#include <string>

namespace detect {

enum class GameProfile {
    Unknown,
    HasDLSS,     // nvngx / nvngx_dlss present -> hijack
    HasXeSS,     // libxess present            -> hijack
    HasFSR,      // ffx_* / amd_fidelityfx     -> upgrade / passthrough
    Bare         // no upscaler -> post-process fallback only
};

struct DetectResult {
    GameProfile profile = GameProfile::Unknown;
    bool dlss = false;
    bool xess = false;
    bool fsr  = false;
    std::string detail;
};

// Scans currently-loaded modules. Call once after the game has booted its
// renderer (we trigger it lazily from the first Present), and optionally re-run,
// since some games load their upscaler DLL late.
DetectResult scan_loaded_modules();

const char* profile_name(GameProfile p);

} // namespace detect
