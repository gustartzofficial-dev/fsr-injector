#include "core/log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <fstream>

namespace core {
namespace {
    std::mutex g_mtx;
    std::ofstream g_file;

    // Portable wide -> narrow path (MSVC's wchar_t* fstream::open is non-standard).
    std::string narrow(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(),
                            s.data(), n, nullptr, nullptr);
        return s;
    }
}

void log_init(const std::wstring& dll_dir) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string path = narrow(dll_dir) + "\\fsr_injector.log";
    g_file.open(path, std::ios::out | std::ios::trunc);
    if (g_file) { g_file << "[fsr-injector] log started\n"; g_file.flush(); }
}

void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) { g_file << msg << '\n'; g_file.flush(); }
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
    if (g_file) g_file.close();
}

} // namespace core
