// SPDX-License-Identifier: LGPL-3.0-or-later
// 奖励窗口（AwardScreen）朗读：标题、正文与"下一关"按钮。
// 该界面不向语义层暴露控件；改经拦截 ABI（tao.interception）Hook 引擎的
// LawnApp::ShowAwardScreen / KillAwardScreen 与 AwardScreen::DrawBottom 获取内容。
// DrawBottom 收到的是本地化键（如 [GOT_SHOVEL]）；译文经宿主导出函数
// TodStringTranslate（见 lib/TaoEngineHost.def）解析：TaoEngineImport.cpp 的
// 延迟加载钩子把符号解析到游戏 exe，分配桥保证返回的 std::string 跨模块
// 释放安全。探测不到导出或查无译文时回退朗读清理后的键名。
#pragma once
#include "HostServices.hpp"
#include <tao/TaoInterceptionAbi.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define TOD_CALL_CONV __cdecl
#else
#define TOD_CALL_CONV
#endif
#include <algorithm>
#include <functional>
#include <string>
#include <string_view>

// 宿主导出函数（Windows 链接 TaoEngineHost.lib，Android 链接 libmain.so）。
std::string TOD_CALL_CONV TodStringTranslate(std::string_view theString);

class Award {
public:
    void SetSay(std::function<void(std::string, bool)> sink) { say = std::move(sink); }
    void SetLog(std::function<void(const std::string&)> sink) { log = std::move(sink); }

    bool Install(const TaoPluginHostV2* host) {
        if (host->query_interface(host->host_context, tao::AbiString(TAO_PLUGIN_INTERCEPTION_INTERFACE),
                TAO_PLUGIN_INTERCEPTION_VERSION_1, &api, sizeof(api)) != TAO_PLUGIN_OK) {
            Log("奖励窗口：tao.interception 接口查询失败");
            return false;
        }
        std::string idShow, idKill, idDraw, sigShow, sigKill, sigDraw;
        for (size_t i = 0;; ++i) {
            TaoInterceptionPointV1 point{};
            point.struct_size = sizeof(point);
            if (api.describe(api.host_context, i, &point) != TAO_PLUGIN_OK) break;
            std::string id = View(point.id);
            std::string sig = View(point.signature);
            if (id.find("LawnApp::ShowAwardScreen") != std::string::npos) { idShow = id; sigShow = sig; }
            else if (id.find("LawnApp::KillAwardScreen") != std::string::npos) { idKill = id; sigKill = sig; }
            else if (id.find("AwardScreen::DrawBottom") != std::string::npos) { idDraw = id; sigDraw = sig; }
        }
        if (idShow.empty() || idKill.empty() || idDraw.empty()) { Log("奖励窗口：未找到所需拦截点"); return false; }
        const bool ok = Hook(idShow, sigShow, &OnShow) && Hook(idKill, sigKill, &OnKill) && Hook(idDraw, sigDraw, &OnDrawBottom);
        if (!ok) Log("奖励窗口：替换安装失败");
        // 探测宿主导出；缺失时不触发延迟加载，朗读回退清理后的键名。
#ifdef _WIN32
        if (const HMODULE exeModule = GetModuleHandleW(nullptr))
            translateReady = GetProcAddress(exeModule, kTranslateExport) != nullptr;
#else
        translateReady = true;
#endif
        if (!translateReady) Log("奖励窗口：宿主未导出 TodStringTranslate，回退键名朗读");
        return ok;
    }

    bool Active() const { return active; }

    void Pump() {
        if (!active || announced) return;
        if (title.empty() && awardText.empty() && message.empty()) return; // 等 DrawBottom 首帧送键
        announced = true;
        std::string text;
        auto append = [&text](const std::string& part) {
            if (part.empty()) return;
            if (!text.empty()) text += "。";
            text += part;
        };
        append(Translate(title));
        if (awardText != title) append(Translate(awardText));
        if (message != awardText) append(Translate(message));
        // AwardType：0=关卡奖励 1=制作名单便条 2=帮助便条 3=仅成就 4=名单前便条
        text += awardType == 2 ? "。按回车返回。" : "。下一关，按回车继续。";
        Say(std::move(text));
    }

private:
    struct SvView { const char* data; size_t size; };

    // TodStringTranslate(std::string_view) 重载的宿主导出修饰名。
    static constexpr char kTranslateExport[] =
        "?TodStringTranslate@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$basic_string_view@DU?$char_traits@D@std@@@2@@Z";

    static std::string View(TaoPluginStringView view) { return {view.data ? view.data : "", view.size}; }
    void Say(std::string text, bool interrupt = true) const { if (say) say(std::move(text), interrupt); }
    void Log(const std::string& text) const { if (log) log(text); }

    bool Hook(const std::string& id, const std::string& signature, TaoInterceptionCallbackV1 callback) {
        TaoInterceptionReplacementV1 spec{};
        spec.struct_size = sizeof(spec);
        spec.id = tao::AbiString(id.c_str());
        spec.signature = tao::AbiString(signature.c_str());
        spec.callback = callback;
        spec.context = this;
        spec.release = nullptr;
        TaoPluginToken token = 0;
        if (api.install(api.host_context, &spec, &token) == TAO_PLUGIN_OK) return true;
        std::string why = "未知原因";
        TaoPluginStringView err{};
        if (api.last_error && api.last_error(api.host_context, &err) == TAO_PLUGIN_OK && err.data && err.size)
            why.assign(err.data, err.size);
        Log("奖励窗口：Hook 安装失败：" + why);
        return false;
    }
    void ReadArg(size_t index, std::string& out, TaoPluginToken token) const {
        void* value = nullptr;
        if (api.argument(api.host_context, token, index, &value) != TAO_PLUGIN_OK || !value) return;
        const auto view = *static_cast<const SvView*>(value);
        if (view.data && view.size && view.size < 4096) out.assign(view.data, view.size);
    }
    // 本地化键 → 译文；未探测到导出、非键文本或查无译文时回退清理后的键名。
    std::string Translate(const std::string& key) const {
        if (translateReady && key.size() >= 2 && key.front() == '[' && key.back() == ']') {
            std::string out = TodStringTranslate(key);
            if (!out.empty() && out != key) return out;
        }
        return CleanKey(key);
    }
    static std::string CleanKey(std::string key) { // 回退：清理 [KEY_NAME] 键名
        if (key.size() >= 2 && key.front() == '[' && key.back() == ']') key = key.substr(1, key.size() - 2);
        std::replace(key.begin(), key.end(), '_', ' ');
        return key;
    }

    static TaoPluginStatus TAO_PLUGIN_CALL OnShow(void* data, TaoPluginToken token) {
        auto& self = *static_cast<Award*>(data);
        void* value = nullptr;
        if (self.api.argument(self.api.host_context, token, 1, &value) == TAO_PLUGIN_OK && value)
            self.awardType = *static_cast<const int*>(value); // 参数 1 = AwardType&
        self.active = true;
        self.title.clear(); self.awardText.clear(); self.message.clear();
        self.announced = false;
        self.api.original(self.api.host_context, token);
        return TAO_PLUGIN_OK;
    }
    static TaoPluginStatus TAO_PLUGIN_CALL OnKill(void* data, TaoPluginToken token) {
        auto& self = *static_cast<Award*>(data);
        self.active = false;
        self.api.original(self.api.host_context, token);
        return TAO_PLUGIN_OK;
    }
    static TaoPluginStatus TAO_PLUGIN_CALL OnDrawBottom(void* data, TaoPluginToken token) {
        auto& self = *static_cast<Award*>(data);
        self.ReadArg(1, self.title, token);
        self.ReadArg(2, self.awardText, token);
        self.ReadArg(3, self.message, token);
        self.api.original(self.api.host_context, token);
        return TAO_PLUGIN_OK;
    }

    TaoPluginInterceptionV1 api{};
    bool active = false;
    int awardType = 0;
    std::string title, awardText, message;
    bool announced = false;
    bool translateReady = false;
    std::function<void(std::string, bool)> say;
    std::function<void(const std::string&)> log;
};
