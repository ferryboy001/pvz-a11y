// SPDX-License-Identifier: LGPL-3.0-or-later
// 挑战选择界面（scene=7）关卡按钮命名：宿主的语义层只暴露裸按钮控件
// （mLabel 为空，名称全丢；且全部页的 72 个按钮同时存在于控件树，仅当前页
// 可用）。这里经拦截 ABI（tao.interception）Hook LawnApp::ShowChallengeScreen /
// KillChallengeScreen 和 ChallengeScreen::UpdateButtons 跟踪当前页码，再按
// 内嵌的 gChallengeDefs 副本（页/行/列 +
// 本地化键，提取自游戏 exe）把焦点控件的 bounds 反查回当前页的关卡索引。
// 译文优先走宿主导出的 TodStringTranslate，失败时回退内嵌中文名表。
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
#include <cmath>
#include <functional>
#include <iterator>
#include <string>

// 宿主导出函数（Windows 链接 TaoEngineHost.lib，Android 链接 libmain.so）。
std::string TOD_CALL_CONV TodStringTranslate(std::string_view theString);
// 借用拦截调用中的原生对象，只读取 SDK 声明的当前页字段。
int A11yChallengePage(const void* screen);

class ChallengeNames {
public:
    void SetLog(std::function<void(const std::string&)> sink) { log = std::move(sink); }

    bool Install(const TaoPluginHostV2* host) {
        if (host->query_interface(host->host_context, tao::AbiString(TAO_PLUGIN_INTERCEPTION_INTERFACE),
                TAO_PLUGIN_INTERCEPTION_VERSION_1, &api, sizeof(api)) != TAO_PLUGIN_OK) {
            Log("挑战关卡命名：tao.interception 接口查询失败");
            return false;
        }
        std::string idShow, idKill, idUpdate, sigShow, sigKill, sigUpdate;
        for (size_t i = 0;; ++i) {
            TaoInterceptionPointV1 point{};
            point.struct_size = sizeof(point);
            if (api.describe(api.host_context, i, &point) != TAO_PLUGIN_OK) break;
            std::string id = View(point.id);
            // 拦截点 id 是 __FUNCSIG__ 风格（含 &Class::Method 字面量）。
            if (id.find("LawnApp::ShowChallengeScreen") != std::string::npos) {
                idShow = id; sigShow = View(point.signature);
            } else if (id.find("LawnApp::KillChallengeScreen") != std::string::npos) {
                idKill = id; sigKill = View(point.signature);
            } else if (id.find("ChallengeScreen::UpdateButtons") != std::string::npos) {
                idUpdate = id; sigUpdate = View(point.signature);
            }
        }
        if (idShow.empty()) { Log("挑战关卡命名：未找到所需拦截点"); return false; }
        bool ok = Hook(idShow, sigShow, &OnShow);
        if (!idKill.empty()) ok = Hook(idKill, sigKill, &OnKill) && ok;
        if (!idUpdate.empty()) ok = Hook(idUpdate, sigUpdate, &OnUpdateButtons) && ok;
        else { Log("挑战关卡命名：未找到页内切换拦截点"); ok = false; }
        if (!ok) Log("挑战关卡命名：替换安装失败");
        // 探测宿主导出；缺失时不触发延迟加载，命名回退内嵌中文名表。
#ifdef _WIN32
        if (const HMODULE exeModule = GetModuleHandleW(nullptr))
            translateReady = GetProcAddress(exeModule, kTranslateExport) != nullptr;
#else
        translateReady = true;
#endif
        if (!translateReady) Log("挑战关卡命名：宿主未导出 TodStringTranslate，回退内嵌名表");
        return ok;
    }

    // 焦点控件 → 关卡名。仅处理挑战选择场景的按钮控件；查不到返回空串。
    std::string Lookup(int scene, uint32_t role, TaoPluginRect bounds) const {
        if (scene != int(tao::enums::GameScenes::SCENE_CHALLENGE.Value())) return {};
        if (role != TAO_ACCESSIBLE_ROLE_BUTTON || currentPage < 0) return {};
        if (std::abs(bounds.width - 104) > 8 || std::abs(bounds.height - 115) > 8) return {};
        for (size_t index = 0; index < std::size(kDefs); ++index) {
            const auto& def = kDefs[index];
            if (def.page != currentPage) continue;
            // 布局公式（对照 F12 DumpTree 实测）：列 x=38+155c；生存页 3 行
            // y=125+145r，其余页 4 行 y=93+119r。
            const int x = 38 + 155 * def.col;
            const int y = def.page == 0 ? 125 + 145 * def.row : 93 + 119 * def.row;
            if (std::abs(int(bounds.x) - x) <= 6 && std::abs(int(bounds.y) - y) <= 6)
                return Resolve(int(index));
        }
        return {};
    }

private:
    struct Def { const char* key; const char* fallback; int page, row, col; };
    // gChallengeDefs 副本：索引 = DrawButton 的 theChallengeIndex（= GameMode-1），
    // 字段提取自游戏 exe 的静态表（20261005 版）。
    static constexpr Def kDefs[] = {
        {"[SURVIVAL_DAY_NORMAL]", "生存模式：白天", 0, 0, 0},
        {"[SURVIVAL_NIGHT_NORMAL]", "生存模式：黑夜", 0, 0, 1},
        {"[SURVIVAL_POOL_NORMAL]", "生存模式：泳池", 0, 0, 2},
        {"[SURVIVAL_FOG_NORMAL]", "生存模式：浓雾", 0, 0, 3},
        {"[SURVIVAL_ROOF_NORMAL]", "生存模式：屋顶", 0, 0, 4},
        {"[SURVIVAL_DAY_HARD]", "生存模式：白天困难", 0, 1, 0},
        {"[SURVIVAL_NIGHT_HARD]", "生存模式：黑夜困难", 0, 1, 1},
        {"[SURVIVAL_POOL_HARD]", "生存模式：泳池困难", 0, 1, 2},
        {"[SURVIVAL_FOG_HARD]", "生存模式：浓雾困难", 0, 1, 3},
        {"[SURVIVAL_ROOF_HARD]", "生存模式：屋顶困难", 0, 1, 4},
        {"[SURVIVAL_DAY_ENDLESS]", "生存模式：白天无尽", 2, 3, 0},
        {"[SURVIVAL_NIGHT_ENDLESS]", "生存模式：黑夜无尽", 2, 3, 1},
        {"[SURVIVAL_POOL_ENDLESS]", "生存模式：泳池无尽", 0, 2, 2},
        {"[SURVIVAL_FOG_ENDLESS]", "生存模式：浓雾无尽", 2, 3, 2},
        {"[SURVIVAL_ROOF_ENDLESS]", "生存模式：屋顶无尽", 2, 3, 3},
        {"[WAR_AND_PEAS]", "植物僵尸", 1, 0, 0},
        {"[WALL_NUT_BOWLING]", "坚果保龄球", 1, 0, 1},
        {"[SLOT_MACHINE]", "老虎机", 1, 0, 2},
        {"[ITS_RAINING_SEEDS]", "种子雨", 1, 0, 3},
        {"[BEGHOULED]", "宝石迷阵", 1, 0, 4},
        {"[INVISIGHOUL]", "隐形食脑者", 1, 1, 0},
        {"[SEEING_STARS]", "观星", 1, 1, 1},
        {"[ZOMBIQUARIUM]", "僵尸水族馆", 1, 1, 2},
        {"[BEGHOULED_TWIST]", "宝石迷阵转转看", 1, 1, 3},
        {"[LITTLE_TROUBLE]", "小僵尸大麻烦", 1, 1, 4},
        {"[PORTAL_COMBAT]", "传送门", 1, 2, 0},
        {"[COLUMN_AS_YOU_SEE_EM]", "排山倒海", 1, 2, 1},
        {"[BOBSLED_BONANZA]", "雪橇区", 1, 2, 2},
        {"[ZOMBIES_ON_SPEED]", "僵尸快跑", 1, 2, 3},
        {"[WHACK_A_ZOMBIE]", "打僵尸", 1, 2, 4},
        {"[LAST_STAND]", "谁笑到最后", 1, 3, 0},
        {"[WAR_AND_PEAS_2]", "植物僵尸 2", 1, 3, 1},
        {"[WALL_NUT_BOWLING_EXTREME]", "坚果保龄球 2", 1, 3, 2},
        {"[POGO_PARTY]", "蹦蹦舞会", 1, 3, 3},
        {"[FINAL_BOSS]", "僵王博士的复仇", 1, 3, 4},
        {"[ART_CHALLENGE_WALL_NUT]", "坚果艺术锦标赛", 2, 0, 0},
        {"[SUNNY_DAY]", "晴天", 2, 0, 1},
        {"[UNSODDED]", "无草皮之地", 2, 0, 2},
        {"[BIG_TIME]", "重要时刻", 2, 0, 3},
        {"[ART_CHALLENGE_SUNFLOWER]", "向日葵艺术锦标赛", 2, 0, 4},
        {"[AIR_RAID]", "空袭", 2, 1, 0},
        {"[ICE_LEVEL]", "冰冻关卡", 2, 1, 1},
        {"[ZEN_GARDEN]", "禅境花园", 2, 1, 2},
        {"[HIGH_GRAVITY]", "高重力", 2, 1, 3},
        {"[GRAVE_DANGER]", "坟墓危机", 2, 1, 4},
        {"[CAN_YOU_DIG_IT]", "挖土挑战", 2, 2, 0},
        {"[DARK_STORMY_NIGHT]", "暴风雨夜", 2, 2, 1},
        {"[BUNGEE_BLITZ]", "蹦极闪电战", 2, 2, 2},
        {"Squirrel", "松鼠", 2, 2, 3},
        {"Tree of Wisdom", "智慧树", 2, 2, 4},
        {"[SCARY_POTTER_1]", "破罐者 1", 3, 0, 0},
        {"[SCARY_POTTER_2]", "破罐者 2", 3, 0, 1},
        {"[SCARY_POTTER_3]", "破罐者 3", 3, 0, 2},
        {"[SCARY_POTTER_4]", "破罐者 4", 3, 0, 3},
        {"[SCARY_POTTER_5]", "破罐者 5", 3, 0, 4},
        {"[SCARY_POTTER_6]", "破罐者 6", 3, 1, 0},
        {"[SCARY_POTTER_7]", "破罐者 7", 3, 1, 1},
        {"[SCARY_POTTER_8]", "破罐者 8", 3, 1, 2},
        {"[SCARY_POTTER_9]", "破罐者 9", 3, 1, 3},
        {"[SCARY_POTTER_ENDLESS]", "破罐者无尽版", 3, 1, 4},
        {"[I_ZOMBIE_1]", "我是僵尸 1", 3, 2, 0},
        {"[I_ZOMBIE_2]", "我是僵尸 2", 3, 2, 1},
        {"[I_ZOMBIE_3]", "我是僵尸 3", 3, 2, 2},
        {"[I_ZOMBIE_4]", "我是僵尸 4", 3, 2, 3},
        {"[I_ZOMBIE_5]", "我是僵尸 5", 3, 2, 4},
        {"[I_ZOMBIE_6]", "我是僵尸 6", 3, 3, 0},
        {"[I_ZOMBIE_7]", "我是僵尸 7", 3, 3, 1},
        {"[I_ZOMBIE_8]", "我是僵尸 8", 3, 3, 2},
        {"[I_ZOMBIE_9]", "我是僵尸 9", 3, 3, 3},
        {"[I_ZOMBIE_ENDLESS]", "我是僵尸无尽版", 3, 3, 4},
        {"Upsell", "推广页", 2, 3, 4},
    };

    // TodStringTranslate(std::string_view) 重载的宿主导出修饰名。
    static constexpr char kTranslateExport[] =
        "?TodStringTranslate@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$basic_string_view@DU?$char_traits@D@std@@@2@@Z";

    static std::string View(TaoPluginStringView view) { return {view.data ? view.data : "", view.size}; }
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
        Log("挑战关卡命名：Hook 安装失败：" + why);
        return false;
    }
    // 本地化键 → 译文；未探测到导出、非键文本或查无译文时回退内嵌中文名。
    std::string Resolve(int index) const {
        const auto& def = kDefs[index];
        const std::string key = def.key;
        if (translateReady && key.size() >= 2 && key.front() == '[' && key.back() == ']') {
            std::string out = TodStringTranslate(key);
            if (!out.empty() && out != key) return out;
        }
        return def.fallback;
    }

    static TaoPluginStatus TAO_PLUGIN_CALL OnShow(void* data, TaoPluginToken token) {
        auto& self = *static_cast<ChallengeNames*>(data);
        void* value = nullptr; // 参数 1 = ChallengePage thePage
        if (self.api.argument(self.api.host_context, token, 1, &value) == TAO_PLUGIN_OK && value)
            self.currentPage = *static_cast<const int*>(value);
        self.api.original(self.api.host_context, token);
        self.Log("挑战关卡命名：进入挑战选择，页码 " + std::to_string(self.currentPage));
        return TAO_PLUGIN_OK;
    }
    static TaoPluginStatus TAO_PLUGIN_CALL OnKill(void* data, TaoPluginToken token) {
        auto& self = *static_cast<ChallengeNames*>(data);
        self.currentPage = -1;
        self.api.original(self.api.host_context, token);
        return TAO_PLUGIN_OK;
    }
    static TaoPluginStatus TAO_PLUGIN_CALL OnUpdateButtons(void* data, TaoPluginToken token) {
        auto& self = *static_cast<ChallengeNames*>(data);
        void* value = nullptr; // 参数 0 = ChallengeScreen* const&
        if (self.api.argument(self.api.host_context, token, 0, &value) == TAO_PLUGIN_OK && value) {
            const int page = A11yChallengePage(*static_cast<void* const*>(value));
            if (page >= 0 && page <= 3 && page != self.currentPage) {
                self.currentPage = page;
                self.Log("挑战关卡命名：页内切换，页码 " + std::to_string(page));
            }
        }
        self.api.original(self.api.host_context, token);
        return TAO_PLUGIN_OK;
    }

    TaoPluginInterceptionV1 api{};
    int currentPage = -1;
    bool translateReady = false;
    std::function<void(const std::string&)> log;
};
