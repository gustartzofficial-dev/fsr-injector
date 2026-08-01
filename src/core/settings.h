#pragma once
#include <string>

// Unified runtime settings.
//
// Values are resolved in priority order:
//   1. Environment variable (e.g. FSRINJ_DX12_SHARPNESS=0.4) -- highest priority
//   2. fsrinj.ini next to the injector DLL (key names match the env var names)
//   3. The compiled-in fallback passed by the caller
//
// The INI format is deliberately trivial: "KEY=VALUE" lines, '#' or ';' comments,
// [sections] ignored. See fsrinj.ini.example in the repo root.
namespace core::settings {

// Parse fsrinj.ini from the given directory (the DLL directory). Safe to call
// once at startup; missing file is fine.
void init(const std::wstring& dll_dir);

// True if the key is set via env var or INI (regardless of value).
bool has(const wchar_t* key);

// Typed getters. Env var wins over INI; fallback used when neither is present
// or the value fails to parse. Float/int results are clamped to [lo, hi].
bool  get_bool(const wchar_t* key, bool fallback);
float get_float(const wchar_t* key, float fallback, float lo, float hi);
int   get_int(const wchar_t* key, int fallback, int lo, int hi);

} // namespace core::settings
