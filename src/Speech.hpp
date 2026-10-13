// SPDX-License-Identifier: LGPL-3.0-or-later
// 语音输出：优先 prism（自动协商 NVDA/JAWS/SAPI 等读屏后端，对应 PvZA11y 的 Tolk 通道），
// prism.dll 缺失时回退到 Windows SAPI。prism.dll / tolk.dll 与 plugin.dll 同目录部署，
// 运行时按插件自身路径显式加载，不依赖游戏目录或 PATH。
#pragma once
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef PVZ_A11Y_WITH_PRISM
#ifndef PRISM_STATIC
#define PRISM_STATIC
#endif
#include <prism.h>
#endif
#include <deque>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <sapi.h>
#endif
#ifdef __ANDROID__
#include "AndroidJniBridge.hpp"
#endif

#ifdef _WIN32
// 解析插件自身模块句柄用的本模块符号锚点
inline void ModuleAnchor() {}
#endif

class Speech {
public:
    bool Start() {
        if (worker.joinable()) return BackendName() != "none";
        closing = false;
        std::promise<bool> ready;
        auto result = ready.get_future();
        worker = std::thread([this, ready = std::move(ready)]() mutable {
            const bool started = StartInternal();
            PublishBackend();
            ready.set_value(started);
            Run();
            CloseInternal();
        });
        return result.get();
    }
    const std::string& Diagnostics() const { return diag; }
    std::string Pinned() const { std::lock_guard lock(queueMutex); return pinned; }
    std::string BackendName() const { std::lock_guard lock(queueMutex); return backendName; }
    std::string TakeBackendChange() {
        std::lock_guard lock(queueMutex);
        auto name = std::move(backendChange); backendChange.clear(); return name;
    }
    void Settings(int volume, int rate) { Enqueue({Kind::Settings, {}, false, volume, rate}); }
    void StopSpeaking() { Enqueue({Kind::Stop}); }
    void SetForceSapi(bool force) { Enqueue({Kind::Force, {}, force}); }
    void Say(std::string text, bool interrupt) {
        if (!text.empty()) Enqueue({Kind::Say, std::move(text), interrupt});
    }
    // 所有检测、初始化、播报和 COM 对象都归语音线程所有；游戏 tick 不等待读屏 API。
    void Pump() {}
    void Close() {
        if (!worker.joinable()) return;
        { std::lock_guard lock(queueMutex); closing = true; commands.clear(); }
        wake.notify_one();
        worker.join();
    }
    ~Speech() { Close(); }

private:
    friend struct SpeechTestAccess;
    enum class Kind { Say, Settings, Stop, Force };
    struct Command {
        Kind kind;
        std::string text;
        bool flag = false;
        int volume = 80, rate = 0;
    };
    void Enqueue(Command command) {
        std::lock_guard lock(queueMutex);
        if (!worker.joinable() || closing) return;
        if (command.kind == Kind::Stop || (command.kind == Kind::Say && command.flag)) {
            std::erase_if(commands, [](const Command& c) { return c.kind == Kind::Say; });
        } else if (command.kind == Kind::Say) {
            const auto count = std::count_if(commands.begin(), commands.end(), [](const Command& c) { return c.kind == Kind::Say; });
            if (count >= 4) commands.erase(std::find_if(commands.begin(), commands.end(), [](const Command& c) { return c.kind == Kind::Say; }));
        } else if (command.kind == Kind::Settings) {
            std::erase_if(commands, [](const Command& c) { return c.kind == Kind::Settings; });
        }
        commands.push_back(std::move(command));
        wake.notify_one();
    }
    void Run() {
        // 自行在语音线程每秒复查，避免随包 prism 0.18.2 的 availability enumerator
        // 在 shutdown 时访问冲突；无变化时保留实例，API 尚未就绪时下次继续重试。
        auto nextProbe = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        for (;;) {
            Command command{Kind::Stop};
            bool hasCommand = false;
            {
                std::unique_lock lock(queueMutex);
                auto deadline = nextProbe;
                if (!pending.empty()) deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(20));
                wake.wait_until(lock, deadline, [this] { return closing || !commands.empty(); });
                if (closing) break;
                if (!commands.empty()) { command = std::move(commands.front()); commands.pop_front(); hasCommand = true; }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextProbe) {
                RefreshPrism();
                nextProbe = now + std::chrono::seconds(1);
            }
            if (hasCommand) {
                switch (command.kind) {
                case Kind::Say: SayInternal(std::move(command.text), command.flag); break;
                case Kind::Settings: SettingsInternal(command.volume, command.rate); break;
                case Kind::Stop: StopInternal(); lastQueued.clear(); break;
                case Kind::Force: ForceSapiInternal(command.flag); PublishBackend(); break;
                }
            }
            PumpInternal();
        }
    }
    void PublishBackend() {
        std::string name = active == Backend::Sapi ? "sapi" : "none";
#ifdef PVZ_A11Y_WITH_PRISM
        if (active == Backend::Prism && prismBackend) name = std::string("prism/") + prism.name(prismBackend);
#endif
        std::lock_guard lock(queueMutex);
        if (backendName != name) backendChange = name;
        backendName = std::move(name);
    }
    bool StartInternal() {
        if (StartPrism()) { active = Backend::Prism; return true; }
#ifdef _WIN32
        if (StartSapi()) { active = Backend::Sapi; return true; }
#endif
        return false;
    }
    void SettingsInternal(int volume, int rate) {
        savedVolume = volume; savedRate = rate;
#ifdef PVZ_A11Y_WITH_PRISM
        if (active == Backend::Prism) {
            // prism 的音量/语速是 [0.0, 1.0] 归一化区间
            prism.set_volume(prismBackend, volume / 100.0f);
            prism.set_rate(prismBackend, (rate + 10) / 20.0f);
        }
#endif
#ifdef _WIN32
        if (active == Backend::Sapi && voice) {
            voice->SetVolume(static_cast<USHORT>(volume));
            voice->SetRate(rate);
        }
#endif
    }
    void StopInternal() {
        pending.clear();
#ifdef PVZ_A11Y_WITH_PRISM
        if (active == Backend::Prism && prismBackend) prism.stop(prismBackend);
#endif
#ifdef _WIN32
        if (active == Backend::Sapi && voice) voice->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
#endif
    }
    // 临时诊断：强制切到 SAPI 输出（prism 保持检测，但不经读屏播报），
    // 用于排查读屏 API（ZDSR 等）在特定场景与游戏冲突导致的崩溃；解除后切回 prism。
    void ForceSapiInternal(bool force) {
        if (force == forceSapi) return;
        forceSapi = force;
#if defined(_WIN32) && defined(PVZ_A11Y_WITH_PRISM)
        if (!force) RefreshPrism();
        StopInternal(); lastQueued.clear();
        if (force) {
            active = (voice || StartSapi()) ? Backend::Sapi : Backend::None;
        } else if (prismBackend) {
            active = Backend::Prism;
        }
#endif
        SettingsInternal(savedVolume, savedRate);
    }
    void SayInternal(std::string text, bool interrupt) {
        if (text.empty()) return;
        if (interrupt) { StopInternal(); lastQueued.clear(); }
        else if (text == lastQueued) return;
        lastQueued = text;
#ifdef PVZ_A11Y_WITH_PRISM
        if (active == Backend::Prism) {
            if (prism.speak(prismBackend, text.c_str(), interrupt) == PRISM_OK) return;
            // 检测通知到达前旧读屏已退出：重新协商并在新后端重试当前文本一次。
            RefreshPrism(true);
            if (active == Backend::Prism) { prism.speak(prismBackend, text.c_str(), interrupt); return; }
        }
#endif
#ifdef _WIN32
        if (active == Backend::Sapi) {
            if (pending.size() == 4) pending.pop_front();
            pending.push_back(std::move(text));
            PumpInternal();
        }
#endif
    }
    void PumpInternal() {
#ifdef _WIN32
        if (active != Backend::Sapi || !voice || pending.empty()) return;
        SPVOICESTATUS status{};
        if (FAILED(voice->GetStatus(&status, nullptr)) || status.dwRunningState == SPRS_IS_SPEAKING) return;
        auto text = std::move(pending.front()); pending.pop_front();
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0) return;
        std::wstring wide(length, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length);
        voice->Speak(wide.c_str(), SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
#endif
    }
    void CloseInternal() {
        StopInternal();
#ifdef PVZ_A11Y_WITH_PRISM
        if (prismBackend) { prism.backend_free(prismBackend); prismBackend = nullptr; }
        if (prismContext) { prism.shutdown(prismContext); prismContext = nullptr; }
#endif
#ifdef _WIN32
#ifdef PVZ_A11Y_WITH_PRISM
        if (prism.module) { FreeLibrary(prism.module); prism = {}; }
#endif
        if (voice) { voice->Release(); voice = nullptr; }
        if (uninitialize) { CoUninitialize(); uninitialize = false; }
#endif
        active = Backend::None;
        forceSapi = false; lastQueued.clear(); diag.clear();
        PublishBackend();
    }
#ifdef PVZ_A11Y_WITH_PRISM
    struct PrismApi {
#ifdef _WIN32
        HMODULE module = nullptr;
#endif
        PrismContext*(PRISM_CALL* init)(PrismConfig*) = nullptr;
        void (PRISM_CALL* shutdown)(PrismContext*) = nullptr;
        PrismBackend*(PRISM_CALL* create_best)(PrismContext*) = nullptr;
        size_t (PRISM_CALL* count)(PrismContext*) = nullptr;
        PrismBackendId (PRISM_CALL* id_at)(PrismContext*, size_t) = nullptr;
        PrismBackend* (PRISM_CALL* create)(PrismContext*, PrismBackendId) = nullptr;
        const char* (PRISM_CALL* name)(PrismBackend*) = nullptr;
        uint64_t (PRISM_CALL* features)(PrismBackend*) = nullptr;
        PrismError (PRISM_CALL* initialize)(PrismBackend*) = nullptr;
        PrismError (PRISM_CALL* speak)(PrismBackend*, const char*, bool) = nullptr;
        PrismError (PRISM_CALL* stop)(PrismBackend*) = nullptr;
        void (PRISM_CALL* backend_free)(PrismBackend*) = nullptr;
        PrismError (PRISM_CALL* set_volume)(PrismBackend*, float) = nullptr;
        PrismError (PRISM_CALL* set_rate)(PrismBackend*, float) = nullptr;
    };
#endif

    void RefreshPrism(bool replaceCurrent = false) {
#if defined(PVZ_A11Y_WITH_PRISM)
        if (!prismContext) return;
        PrismBackend* replacement = nullptr;
        // 注册表按优先级降序排列。检查运行时可用性，不能复用旧实例的初始化成功状态。
        for (size_t i = 0, n = prism.count(prismContext); i < n; ++i) {
            auto* candidate = prism.create(prismContext, prism.id_at(prismContext, i));
            if (!candidate) continue;
            const auto features = prism.features(candidate);
#ifdef _WIN32
            PinScreenReaderDlls();
#endif
            if ((features & (PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME | PRISM_BACKEND_SUPPORTS_SPEAK)) !=
                (PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME | PRISM_BACKEND_SUPPORTS_SPEAK)) {
                prism.backend_free(candidate); continue;
            }
            if (!replaceCurrent && prismBackend && std::string(prism.name(candidate)) == prism.name(prismBackend)) {
                prism.backend_free(candidate); return;
            }
            const auto err = prism.initialize(candidate);
#ifdef _WIN32
            PinScreenReaderDlls();
#endif
            if (err == PRISM_OK || err == PRISM_ERROR_ALREADY_INITIALIZED) { replacement = candidate; break; }
            prism.backend_free(candidate);
        }
        if (!replacement && !prismBackend) return;
        StopInternal();
        if (prismBackend) prism.backend_free(prismBackend);
        prismBackend = replacement;
        if (replacement && !forceSapi) active = Backend::Prism;
#if defined(_WIN32)
        else active = (voice || StartSapi()) ? Backend::Sapi : Backend::None;
#else
        else active = Backend::None;
#endif
        SettingsInternal(savedVolume, savedRate);
        lastQueued.clear();
        PublishBackend();
#endif
    }
    bool StartPrism() {
#if defined(PVZ_A11Y_WITH_PRISM)
#if defined(__ANDROID__)
        if (!pvz_a11y::AndroidJniBridge::Initialize()) {
            diag = "prism: Android JNI 初始化失败 (SDL_GetAndroidJNIEnv 为空)";
            return false;
        }
        prism.init = ::prism_init;
        prism.shutdown = ::prism_shutdown;
        prism.create_best = ::prism_registry_create_best;
        prism.count = ::prism_registry_count;
        prism.id_at = ::prism_registry_id_at;
        prism.create = ::prism_registry_create;
        prism.name = ::prism_backend_name;
        prism.features = ::prism_backend_get_features;
        prism.initialize = ::prism_backend_initialize;
        prism.speak = ::prism_backend_speak;
        prism.stop = ::prism_backend_stop;
        prism.backend_free = ::prism_backend_free;
        prism.set_volume = ::prism_backend_set_volume;
        prism.set_rate = ::prism_backend_set_rate;

        prismContext = prism.init(nullptr);
        if (!prismContext) {
            diag = "prism: prism_init 返回空上下文";
            return false;
        }
        prismBackend = prism.create_best(prismContext);
        if (prismBackend) {
            const PrismError err = prism.initialize(prismBackend);
            if (err != PRISM_OK && err != PRISM_ERROR_ALREADY_INITIALIZED) {
                diag = "prism: 后端 initialize 失败 err=" + std::to_string(static_cast<int>(err));
                prism.backend_free(prismBackend);
                prismBackend = nullptr;
            }
        }
        if (!prismBackend) {
            if (diag.empty()) diag = "prism: create_best 无可用后端 (Android读屏/TTS均未就绪)";
            return false;
        }
        active = Backend::Prism;
        return true;
#elif defined(_WIN32)
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ModuleAnchor), &self)) {
            diag = "prism: GetModuleHandleExW 失败 err=" + std::to_string(GetLastError());
            return false;
        }
        // 动态扩容缓冲，避免游戏装在超长路径下 MAX_PATH 截断导致静默回退 SAPI。
        std::wstring path(260, L'\0');
        DWORD length = 0;
        for (;;) {
            length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
            if (!length) {
                diag = "prism: GetModuleFileNameW 失败 err=" + std::to_string(GetLastError());
                return false;
            }
            if (length < path.size() - 1) { path.resize(length); break; }
            if (path.size() >= 32768) { diag = "prism: 插件路径过长"; return false; }
            path.resize(path.size() * 2);
        }
        const std::wstring dll = path.substr(0, path.find_last_of(L"\\/") + 1) + L"prism.dll";
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            diag = "prism: 未找到 " + Narrow(dll) + "（打包缺文件或被安全软件隔离）err=" + std::to_string(GetLastError());
            return false;
        }
        prism.module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!prism.module) {
            diag = "prism: LoadLibraryExW 失败 " + Narrow(dll) + " err=" + std::to_string(GetLastError());
            return false;
        }
        auto get = [this](const char* name) { return GetProcAddress(prism.module, name); };
        prism.init = reinterpret_cast<decltype(prism.init)>(get("prism_init"));
        prism.shutdown = reinterpret_cast<decltype(prism.shutdown)>(get("prism_shutdown"));
        prism.create_best = reinterpret_cast<decltype(prism.create_best)>(get("prism_registry_create_best"));
        prism.count = reinterpret_cast<decltype(prism.count)>(get("prism_registry_count"));
        prism.id_at = reinterpret_cast<decltype(prism.id_at)>(get("prism_registry_id_at"));
        prism.create = reinterpret_cast<decltype(prism.create)>(get("prism_registry_create"));
        prism.name = reinterpret_cast<decltype(prism.name)>(get("prism_backend_name"));
        prism.features = reinterpret_cast<decltype(prism.features)>(get("prism_backend_get_features"));
        prism.initialize = reinterpret_cast<decltype(prism.initialize)>(get("prism_backend_initialize"));
        prism.speak = reinterpret_cast<decltype(prism.speak)>(get("prism_backend_speak"));
        prism.stop = reinterpret_cast<decltype(prism.stop)>(get("prism_backend_stop"));
        prism.backend_free = reinterpret_cast<decltype(prism.backend_free)>(get("prism_backend_free"));
        prism.set_volume = reinterpret_cast<decltype(prism.set_volume)>(get("prism_backend_set_volume"));
        prism.set_rate = reinterpret_cast<decltype(prism.set_rate)>(get("prism_backend_set_rate"));
        const char* missing = nullptr;
        if (!prism.init) missing = "prism_init";
        else if (!prism.shutdown) missing = "prism_shutdown";
        else if (!prism.create_best) missing = "prism_registry_create_best";
        else if (!prism.count) missing = "prism_registry_count";
        else if (!prism.id_at) missing = "prism_registry_id_at";
        else if (!prism.create) missing = "prism_registry_create";
        else if (!prism.name) missing = "prism_backend_name";
        else if (!prism.features) missing = "prism_backend_get_features";
        else if (!prism.initialize) missing = "prism_backend_initialize";
        else if (!prism.speak) missing = "prism_backend_speak";
        else if (!prism.stop) missing = "prism_backend_stop";
        else if (!prism.backend_free) missing = "prism_backend_free";
        else if (!prism.set_volume) missing = "prism_backend_set_volume";
        else if (!prism.set_rate) missing = "prism_backend_set_rate";
        if (missing) {
            diag = std::string("prism: 导出符号缺失 ") + missing + "（prism.dll 版本不匹配）";
            FreeLibrary(prism.module); prism = {};
            return false;
        }
        // 不开启 prism 自带的后台枚举；检测由本类拥有的语音线程完成。
        prismContext = prism.init(nullptr);
        if (!prismContext) {
            diag = "prism: prism_init 返回空上下文";
            FreeLibrary(prism.module); prism = {};
            return false;
        }
        prismBackend = prism.create_best(prismContext);
        PinScreenReaderDlls();
        if (prismBackend) {
            // create_best 返回的后端 prism 已做过初始化（ensure_initialized）；重复 initialize 时
            // ZDSR 后端容忍返回 OK，但 OneCore/SAPI/NVDA 等后端返回 ALREADY_INITIALIZED。
            // prism 官方文档（backend-functions.md）说明此错误非致命，应视为成功，
            // 否则无读屏环境的机器会错误回退到 SAPI（对方日志 err=15 即此情况）。
            const PrismError err = prism.initialize(prismBackend);
            if (err != PRISM_OK && err != PRISM_ERROR_ALREADY_INITIALIZED) {
                diag = "prism: 后端 initialize 失败 err=" + std::to_string(static_cast<int>(err));
                prism.backend_free(prismBackend);
                prismBackend = nullptr;
            }
        }
        if (!prismBackend) {
            if (diag.empty()) diag = "prism: create_best 无可用后端";
            // 保留上下文，读屏稍后启动时仍可从 SAPI 自动切回 prism。
            return false;
        }
        PinScreenReaderDlls();
        return true;
#endif
#else
        diag = "prism: 编译时未启用（构建机 PRISM_ROOT 缺失）";
        return false;
#endif
    }
#ifdef _WIN32
    // prism 的读屏后端经延迟加载引入的 API DLL（如 ZDSRAPI_x64.dll）会在进程内
    // 安装键盘钩子；其一旦被卸载而钩子残留，下次按键会跳到已释放的代码执行
    // （WER 特征：ZDSRAPI_x64.dll_unloaded，BEX64 c0000005）。钉住后进程退出前
    // 永不卸载，杜绝此类崩溃。未使用的后端 DLL 未被加载，跳过即可。
    void PinScreenReaderDlls() {
        for (const wchar_t* name : {L"ZDSRAPI_x64.dll", L"byctrl-x64.dll", L"PCTKUSR.dll"}) {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &module) || !module) continue;
            std::lock_guard lock(queueMutex);
            if (pinned.find(Narrow(name)) != std::string::npos) continue;
            if (!pinned.empty()) pinned += ", ";
            pinned += Narrow(name);
        }
    }
    static std::string Narrow(const std::wstring& wide) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
        return out;
    }
    bool StartSapi() {
        if (voice) return true;
        if (!uninitialize) {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            uninitialize = SUCCEEDED(hr);
            if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
        }
        return SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_ISpVoice,
            reinterpret_cast<void**>(&voice)));
    }
#endif

    enum class Backend { None, Prism, Sapi } active = Backend::None;
    bool forceSapi = false;
    int savedVolume = 80, savedRate = 0;
    std::thread worker;
    mutable std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<Command> commands;
    bool closing = false;
    std::string backendName = "none";
    std::string backendChange;
    std::string lastQueued;
    std::string diag;
    std::string pinned;
#ifdef PVZ_A11Y_WITH_PRISM
    PrismApi prism;
    PrismContext* prismContext = nullptr;
    PrismBackend* prismBackend = nullptr;
#endif
#ifdef _WIN32
    ISpVoice* voice = nullptr;
    bool uninitialize = false;
#endif
    std::deque<std::string> pending;
};
