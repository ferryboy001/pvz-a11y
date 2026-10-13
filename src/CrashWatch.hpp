// SPDX-License-Identifier: LGPL-3.0-or-later
// 崩溃定位（临时诊断）：进程内向量化异常记录器 + 干净退出标记。
// 致命异常（AV/栈溢出/fail-fast）先记 RIP 与模块偏移再放行（不干扰处理链）；
// 若进程退出而日志没有任何异常与 atexit 记录，则是被外部 TerminateProcess。
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

class CrashWatch {
public:
    void Install() {
        if (installed) return;
        installed = true;
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        logPath = exePath;
        const auto slash = logPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) logPath.resize(slash);
        logPath += L"\\crashwatch.log";
        AddVectoredExceptionHandler(1, &CrashWatch::Handler);
        std::atexit(&CrashWatch::CleanExit);
        Write("记录器已安装");
    }

private:
    static void Write(const std::string& line) {
        if (logPath.empty()) return;
        if (FILE* f = _wfopen(logPath.c_str(), L"a+, ccs=UTF-8")) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            fprintf(f, "%02d:%02d:%02d.%03d %s\n", now.wHour, now.wMinute, now.wSecond,
                now.wMilliseconds, line.c_str());
            fclose(f);
        }
    }
    static void CleanExit() { Write("atexit：进程正常退出路径"); }
    static LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code != 0xC0000005 && code != 0xC00000FD && code != 0xC0000409 && code != 0xC000001D)
            return EXCEPTION_CONTINUE_SEARCH;
        std::string where = "?";
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress), &mod)) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(mod, path, MAX_PATH);
            const unsigned long long offset = reinterpret_cast<const char*>(ep->ExceptionRecord->ExceptionAddress) -
                reinterpret_cast<const char*>(mod);
            const std::wstring wide(path);
            const auto slash = wide.find_last_of(L"\\/");
            const std::wstring file = slash != std::wstring::npos ? wide.substr(slash + 1) : wide;
            char name[256]{};
            WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, name, sizeof(name) - 1, nullptr, nullptr);
            char buf[320]{};
            snprintf(buf, sizeof(buf), "%s+0x%llx", name, offset);
            where = buf;
        }
        char line[400]{};
        snprintf(line, sizeof(line), "异常 0x%08lx @ %s (tid=%lu)", code, where.c_str(), GetCurrentThreadId());
        Write(line);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool installed = false;
    static std::wstring logPath;
};
std::wstring CrashWatch::logPath; // 头文件内单一定义：本 mod 只有一个编译单元
