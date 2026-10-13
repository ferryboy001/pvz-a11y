// SPDX-License-Identifier: LGPL-3.0-or-later
// 崩溃前置钩子（诊断即弃）：c0000409 异常数据 5 = FAST_FAIL_FATAL_APP_EXIT，即 CRT
// abort()/无效参数快失败。进程内 VEH/异常处理器在本宿主不可行（插件加
// AddVectoredExceptionHandler 即 __fastfail，见 SDK-feedback 记录），但 abort 走的是
// CRT 钩子而非异常分发：mod 与游戏共用同一个 ucrtbase.dll，signal(SIGABRT)、
// set_terminate、_set_invalid_parameter_handler 都能在快失败前拿到控制权，用
// CaptureStackBackTrace 把调用栈逐帧"模块+RVA"落盘 mods/abort.log（不依赖 Tao，
// 避免在 terminate 上下文里碰宿主回调/日志锁）。
#pragma once
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <atomic>
#include <string>

namespace abortwatch {

inline std::atomic<bool> g_logged{false};

inline void WriteLogLine(FILE* f, const char* kind, size_t i, void* frame) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    uintptr_t rva = reinterpret_cast<uintptr_t>(frame);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frame), &mod) &&
        mod) {
        char path[MAX_PATH] = "";
        if (GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char* slash = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/') slash = p + 1;
            std::snprintf(name, sizeof(name), "%s", slash);
        }
        rva = reinterpret_cast<uintptr_t>(frame) - reinterpret_cast<uintptr_t>(mod);
    }
    std::fprintf(f, "%s frame%02zu %s+0x%zx\n", kind, i, name, rva);
}

// 注意：kind 允许重入（terminate 里再 abort 会触发 SIGABRT 钩子），g_logged 去重。
inline void Capture(const char* kind) {
    if (g_logged.exchange(true)) return;
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos + 1);
    dir += L"mods\\abort.log";
    FILE* f = _wfopen(dir.c_str(), L"a");
    if (!f) return;
    std::fprintf(f, "==== %s ====\n", kind);
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) WriteLogLine(f, kind, i, frames[i]);
    std::fclose(f);
}

inline void __cdecl OnSigAbort(int) {
    Capture("SIGABRT");
    // 返回后 CRT 继续快失败（签名不变），无需再处理。
}

inline void OnTerminate() {
    Capture("TERMINATE");
    std::abort(); // terminate 处理器不允许返回；abort 会再触发 SIGABRT 钩子（已去重）。
}

inline void __cdecl OnInvalidParam(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                                   uintptr_t) {
    Capture("INVALID_PARAM");
    // 返回后 CRT 走 _invoke_watson 快失败。
}

inline void Install() {
    signal(SIGABRT, OnSigAbort);
    std::set_terminate(OnTerminate);
    _set_invalid_parameter_handler(OnInvalidParam);
}

// ---- 死亡方式鉴别（mod 之外无钩子可用：宿主是静态 CRT，其 abort/exit 不经过
// 本 DLL 的 ucrtbase；mod.log 每次会话被覆盖）。alive.log 追加三种痕迹：
//   "static-dtors"  —— mod 静态析构跑了（仅 exit() 路径会跑）
//   "DLL_PROCESS_DETACH" —— 卸载回调跑了（exit()/ExitProcess 都会；硬杀不会）
//   "heartbeat"     —— OnUpdate 存活心跳（附最后 tick/scene）
// 组合即可区分：exit() / ExitProcess / TerminateProcess·快失败。
inline std::atomic<unsigned long long> g_aliveLastTick{0};
inline std::atomic<long> g_aliveLastScene{-1};

inline const wchar_t* AliveLogPath() {
    static wchar_t path[MAX_PATH] = L"";
    static bool init = [] {
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        const size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) dir.resize(pos + 1);
        dir += L"mods\\alive.log";
        wcscpy_s(path, dir.c_str());
        return true;
    }();
    (void)init;
    return path;
}

inline void AppendNote(const char* what) {
    HANDLE h = CreateFileW(AliveLogPath(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER end{};
    SetFilePointerEx(h, {}, &end, FILE_END);
    char buf[160];
    const int n = std::snprintf(buf, sizeof buf, "%lu %s tick=%llu scene=%ld\r\n",
                                GetTickCount(), what,
                                (unsigned long long)g_aliveLastTick.load(),
                                (long)g_aliveLastScene.load());
    DWORD wr = 0;
    WriteFile(h, buf, (DWORD)n, &wr, nullptr);
    CloseHandle(h);
}

struct ExitProbe { // 全局静态对象：exit() 路径析构；ExitProcess/硬杀不析构
    ~ExitProbe() { AppendNote("static-dtors"); }
};

} // namespace abortwatch

#else

#include <atomic>

namespace abortwatch {
inline std::atomic<unsigned long long> g_aliveLastTick{0};
inline std::atomic<long> g_aliveLastScene{-1};
inline void Install() {}
inline void AppendNote(const char*) {}
struct ExitProbe {};
} // namespace abortwatch

#endif
