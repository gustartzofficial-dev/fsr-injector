#include "core/settings.h"
#include "core/log.h"

#include <windows.h>
#include <cwchar>
#include <cwctype>
#include <cstdio>
#include <map>
#include <mutex>

namespace core::settings {
namespace {
    std::mutex g_mtx;
    std::map<std::wstring, std::wstring> g_ini;
    bool g_ini_loaded = false;

    std::wstring upper(std::wstring s) {
        for (auto& c : s) c = (wchar_t)std::towupper(c);
        return s;
    }

    std::wstring trim(const std::wstring& s) {
        size_t b = s.find_first_not_of(L" \t\r\n");
        if (b == std::wstring::npos) return {};
        size_t e = s.find_last_not_of(L" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // Returns true and fills 'out' if the key is set via env var or INI.
    bool raw_value(const wchar_t* key, std::wstring& out) {
        wchar_t buf[128]{};
        DWORD n = GetEnvironmentVariableW(key, buf, 128);
        if (n > 0 && n < 128) { out = buf; return true; }
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_ini.find(upper(key));
        if (it == g_ini.end()) return false;
        out = it->second;
        return true;
    }

    bool parse_bool(const std::wstring& v, bool fallback) {
        if (v.empty()) return fallback;
        wchar_t c = (wchar_t)std::towlower(v[0]);
        if (c == L'1' || c == L't' || c == L'y') return true;
        if (c == L'0' || c == L'f' || c == L'n') return false;
        // "on"/"off"
        if (c == L'o' && v.size() > 1) {
            wchar_t c2 = (wchar_t)std::towlower(v[1]);
            if (c2 == L'n') return true;
            if (c2 == L'f') return false;
        }
        return fallback;
    }
}

void init(const std::wstring& dll_dir) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_ini_loaded) return;
    g_ini_loaded = true;

    std::wstring path = dll_dir + L"\\fsrinj.ini";
    FILE* f = _wfopen(path.c_str(), L"rt");
    if (!f) return; // no INI is fine; env vars / defaults still apply

    char line[512];
    unsigned parsed = 0;
    while (std::fgets(line, sizeof(line), f)) {
        // The INI is expected to be plain ASCII/UTF-8; widen byte-by-byte, which
        // is correct for the ASCII keys/values we define.
        std::wstring wl;
        for (const char* p = line; *p; ++p) wl.push_back((wchar_t)(unsigned char)*p);
        wl = trim(wl);
        if (wl.empty() || wl[0] == L'#' || wl[0] == L';' || wl[0] == L'[') continue;
        size_t eq = wl.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = upper(trim(wl.substr(0, eq)));
        std::wstring val = trim(wl.substr(eq + 1));
        if (key.empty()) continue;
        g_ini[key] = val;
        ++parsed;
    }
    std::fclose(f);
    LOGF("[settings] fsrinj.ini loaded (%u keys)", parsed);
}

bool has(const wchar_t* key) {
    std::wstring v;
    return raw_value(key, v);
}

bool get_bool(const wchar_t* key, bool fallback) {
    std::wstring v;
    if (!raw_value(key, v)) return fallback;
    return parse_bool(trim(v), fallback);
}

float get_float(const wchar_t* key, float fallback, float lo, float hi) {
    std::wstring v;
    float out = fallback;
    if (raw_value(key, v)) {
        wchar_t* end = nullptr;
        float parsed = std::wcstof(v.c_str(), &end);
        if (end != v.c_str()) out = parsed;
    }
    if (out < lo) out = lo;
    if (out > hi) out = hi;
    return out;
}

int get_int(const wchar_t* key, int fallback, int lo, int hi) {
    std::wstring v;
    int out = fallback;
    if (raw_value(key, v)) {
        wchar_t* end = nullptr;
        long parsed = std::wcstol(v.c_str(), &end, 0); // base 0: accepts 0x.. for VK codes
        if (end != v.c_str()) out = (int)parsed;
    }
    if (out < lo) out = lo;
    if (out > hi) out = hi;
    return out;
}

} // namespace core::settings
