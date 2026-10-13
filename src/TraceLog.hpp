// SPDX-License-Identifier: LGPL-3.0-or-later
// 直写文件追踪：宿主的 Log().Info 有缓冲，硬崩溃时内容丢失，无法做事后取证。
// 这里每次一行立即落盘（append + fflush），专供崩溃复现实验。
#pragma once
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>
#include <atomic>
#include <string>

namespace tracefile {
inline std::atomic<bool> enabled{true};
inline bool Enabled() { return enabled.load(std::memory_order_relaxed); }
inline void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }
inline void Write(const std::string& line) {
    if (!Enabled()) return;
#if defined(_WIN32)
    static const std::wstring path = [] {
        wchar_t exe[MAX_PATH] = L"";
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            std::wstring dir(exe);
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                return dir.substr(0, slash) + L"\\mods\\trace.log";
        }
        return std::wstring(L"mods\\trace.log");
    }();
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"a");
    if (!f) return;
    fprintf(f, "%s\n", line.c_str());
    fflush(f);
    fclose(f);
#else
    FILE* f = std::fopen("mods/trace.log", "a");
    if (!f) return;
    std::fprintf(f, "%s\n", line.c_str());
    std::fflush(f);
    std::fclose(f);
#endif
}
} // namespace tracefile
