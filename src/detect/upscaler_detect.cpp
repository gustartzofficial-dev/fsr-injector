#include "detect/upscaler_detect.h"
#include "core/log.h"
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "psapi.lib")

namespace detect {

static bool any_module_matches(const std::vector<std::wstring>& needles) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return false;

    const size_t count = needed / sizeof(HMODULE);
    wchar_t name[MAX_PATH];
    for (size_t i = 0; i < count; ++i) {
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH))
            continue;
        std::wstring n(name);
        std::transform(n.begin(), n.end(), n.begin(),
                       [](wchar_t c){ return (wchar_t)std::towlower(c); });
        for (const auto& needle : needles)
            if (n.find(needle) != std::wstring::npos)
                return true;
    }
    return false;
}

DetectResult scan_loaded_modules() {
    DetectResult r;
    r.dlss = any_module_matches({L"nvngx_dlss", L"nvngx.dll", L"_nvngx"});
    r.xess = any_module_matches({L"libxess"});
    r.fsr  = any_module_matches({L"ffx_", L"amd_fidelityfx", L"amdxc"});

    // Priority: DLSS > XeSS > FSR for hijack quality; bare last.
    if (r.dlss)      r.profile = GameProfile::HasDLSS;
    else if (r.xess) r.profile = GameProfile::HasXeSS;
    else if (r.fsr)  r.profile = GameProfile::HasFSR;
    else             r.profile = GameProfile::Bare;

    r.detail = std::string("dlss=") + (r.dlss?"1":"0") +
               " xess=" + (r.xess?"1":"0") +
               " fsr="  + (r.fsr?"1":"0");
    LOGF("[detect] profile=%s (%s)", profile_name(r.profile), r.detail.c_str());
    return r;
}

const char* profile_name(GameProfile p) {
    switch (p) {
        case GameProfile::HasDLSS: return "HasDLSS";
        case GameProfile::HasXeSS: return "HasXeSS";
        case GameProfile::HasFSR:  return "HasFSR";
        case GameProfile::Bare:    return "Bare";
        default:                   return "Unknown";
    }
}

} // namespace detect
