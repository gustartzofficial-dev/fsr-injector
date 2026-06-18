#pragma once
#include <string>

// Minimal thread-safe logger. Writes to a file next to the DLL so we can debug
// inside games that have no console.
namespace core {

void log_init(const std::wstring& dll_dir);
void log_line(const std::string& msg);
void log_shutdown();

std::string log_format(const char* fmt, ...);
#define LOGF(...) ::core::log_line(::core::log_format(__VA_ARGS__))

} // namespace core
