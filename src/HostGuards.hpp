// SPDX-License-Identifier: LGPL-3.0-or-later
// 宿主崩溃守卫：用宿主自己的 tao.interception 机制拦下观测管线的必崩调用。
// 崩溃均发生在宿主 ObserveAccessibility/PumpRuntimeServices 自身 tick 内
//（mod 回调之外），无法靠停订时机规避，只能拦截入口函数：
//
// 守卫 1（缺陷 D）：LawnApp::DoAlmanacDialog —— 订阅宿主观测期间打开图鉴，
//   AlmanacDialog 构造时观测管线为预览植物计算 grid_x/grid_y 字段，以空棋盘
//   指针调用 Board::GetGridColumns（PDB：Plant::PlantInitialize 插桩 lambda
//   +0x4ac，this=null）。跳过原函数，图鉴不弹出。
//
// 守卫 2（缺陷 E）：Plant::GetNameString(enum SeedType,enum SeedType) ——
//   观测管线经 BoardTargetAt 为棋盘目标（僵尸包 60..74、特殊包）起名时，
//   StrFormat 的 %s 参数是野指针（PDB：GetNameString 插桩 lambda +0x5d，
//   strnlen 读 0x2600000004）。普通植物（0..52）走原实现；其余类型不调
//   original，用 namer 回调（复用 BoardControl::ZombiePacketName）造安全名。
//   该调用走 Invoke<&Plant::GetNameString,…> 注册表模板路由，可被拦截
//   （与缺陷 D 的直接 call 不同）。
// 宿主修复对应空指针/查表缺陷后，各守卫可独立移除。
#pragma once
#include "HostServices.hpp"
#include <tao/TaoInterceptionAbi.h>
#include <functional>
#include <string>
#include <vector>

class HostGuards {
public:
    void SetSay(std::function<void(std::string, bool)> sink) { say = std::move(sink); }
    void SetLog(std::function<void(const std::string&)> sink) { log = std::move(sink); }
    // 僵尸包/特殊包的安全命名（宿主查表对这些类型崩溃/错位）。
    void SetNamer(std::function<std::string(int)> sink) { namer = std::move(sink); }

    bool Install(const TaoPluginHostV2* host) {
        if (host->query_interface(host->host_context, tao::AbiString(TAO_PLUGIN_INTERCEPTION_INTERFACE),
                TAO_PLUGIN_INTERCEPTION_VERSION_1, &api, sizeof(api)) != TAO_PLUGIN_OK) {
            Log("宿主守卫：tao.interception 接口查询失败");
            return false;
        }
        std::string idAlmanac, sigAlmanac, idName, sigName;
        int total = 0;
        for (size_t i = 0;; ++i) {
            TaoInterceptionPointV1 point{};
            point.struct_size = sizeof(point);
            if (api.describe(api.host_context, i, &point) != TAO_PLUGIN_OK) break;
            ++total;
            std::string pid = View(point.id);
            if (idAlmanac.empty() && pid.find("LawnApp::DoAlmanacDialog") != std::string::npos) {
                idAlmanac = pid; sigAlmanac = View(point.signature);
            }
            if (idName.empty() && pid.find("Plant::GetNameString") != std::string::npos) {
                idName = pid; sigName = View(point.signature);
            }
        }
        Log("宿主守卫：拦截点总数 " + std::to_string(total));
        const bool okAlmanac = !idAlmanac.empty() &&
            Install("图鉴守卫", idAlmanac, sigAlmanac, &OnDoAlmanac);
        const bool okName = !idName.empty() &&
            Install(" GetNameString 守卫", idName, sigName, &OnGetNameString);
        if (idName.empty()) Log("宿主守卫：未找到 Plant::GetNameString 拦截点（僵尸包观测崩溃只能靠停订规避）");
        return okAlmanac || okName;
    }

private:
    static std::string View(TaoPluginStringView view) { return {view.data ? view.data : "", view.size}; }
    static void SayText(std::function<void(std::string, bool)>& sink, std::string text) { if (sink) sink(std::move(text), true); }
    void Log(const std::string& text) const { if (log) log(text); }

    bool Install(const char* label, const std::string& id, const std::string& signature, TaoInterceptionCallbackV1 callback) {
        TaoInterceptionReplacementV1 spec{};
        spec.struct_size = sizeof(spec);
        spec.id = tao::AbiString(id.c_str());
        spec.signature = tao::AbiString(signature.c_str());
        spec.callback = callback;
        spec.context = this;
        spec.release = nullptr;
        TaoPluginToken token = 0;
        if (api.install(api.host_context, &spec, &token) != TAO_PLUGIN_OK) {
            std::string why = "未知原因";
            TaoPluginStringView err{};
            if (api.last_error && api.last_error(api.host_context, &err) == TAO_PLUGIN_OK && err.data && err.size)
                why.assign(err.data, err.size);
            Log(std::string(label) + "：Hook 安装失败：" + why);
            return false;
        }
        Log(std::string(label) + "已启用");
        return true;
    }

    // 守卫 1：不调 original，图鉴弹窗完全不构造；结果置空指针。
    static TaoPluginStatus TAO_PLUGIN_CALL OnDoAlmanac(void* data, TaoPluginToken token) {
        auto& self = *static_cast<HostGuards*>(data);
        void* result = nullptr;
        if (self.api.set_result(self.api.host_context, token, &result) != TAO_PLUGIN_OK)
            self.Log("图鉴守卫：set_result 失败（返回默认结果）");
        SayText(self.say, "图鉴暂时无法打开：宿主在订阅无障碍观测期间打开图鉴会崩溃。");
        return TAO_PLUGIN_OK;
    }

    static int ReadInt(HostGuards& self, TaoPluginToken token, size_t index) {
        void* value = nullptr;
        if (self.api.argument(self.api.host_context, token, index, &value) != TAO_PLUGIN_OK || !value) return -1;
        return *static_cast<const int*>(value);
    }

    // 守卫 2：普通植物走原实现；僵尸包/特殊包/越界类型返回安全名，不碰原函数。
    static TaoPluginStatus TAO_PLUGIN_CALL OnGetNameString(void* data, TaoPluginToken token) {
        auto& self = *static_cast<HostGuards*>(data);
        const int type = ReadInt(self, token, 0);
        if (type >= 0 && type < 53) { // 普通植物：原实现（实机验证安全）
            self.api.original(self.api.host_context, token);
            return TAO_PLUGIN_OK;
        }
        std::string name = self.namer ? self.namer(type) : "特殊种子包";
        if (self.api.set_result(self.api.host_context, token, &name) != TAO_PLUGIN_OK)
            self.Log("GetNameString 守卫：set_result 失败 type=" + std::to_string(type));
        return TAO_PLUGIN_OK;
    }

    TaoPluginInterceptionV1 api{};
    std::function<void(std::string, bool)> say;
    std::function<std::string(int)> namer;
    std::function<void(const std::string&)> log;
};
