#include "core/log.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace core {
namespace {
    std::mutex g_mtx;
    FILE* g_file = nullptr;

    // Name of the game executable (without extension), used for the fallback
    // log path so several games don't overwrite each other's logs.
    std::wstring exe_stem() {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p(path);
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p = p.substr(slash + 1);
        size_t dot = p.find_last_of(L'.');
        if (dot != std::wstring::npos) p = p.substr(0, dot);
        return p.empty() ? L"game" : p;
    }

    // Try to open the log for writing at 'path'. Uses the wide-char CRT open so
    // non-ASCII install paths (common outside en-US locales) work correctly --
    // the previous CP_ACP narrow conversion mangled them.
    FILE* try_open(const std::wstring& path) {
        return _wfopen(path.c_str(), L"wt");
    }
}

void log_init(const std::wstring& dll_dir) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) return;

    // Preferred: next to the DLL (= next to the game exe).
    g_file = try_open(dll_dir + L"\\fsr_injector.log");

    // Fallback: %LOCALAPPDATA%\fsr-injector\<exe>.log. Game folders under
    // Program Files are frequently not writable, and losing the log there
    // means losing the only diagnostic channel we have.
    if (!g_file) {
        wchar_t base[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
            std::wstring dir = std::wstring(base) + L"\\fsr-injector";
            CreateDirectoryW(dir.c_str(), nullptr);
            g_file = try_open(dir + L"\\" + exe_stem() + L".log");
        }
    }

    if (g_file) {
        std::fputs("[fsr-injector] log started\n", g_file);
        std::fflush(g_file);
    }
}

void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_file) return;
    std::fputs(msg.c_str(), g_file);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

std::string log_format(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

void log_shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
}

} // namespace core
