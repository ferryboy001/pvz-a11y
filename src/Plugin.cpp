// SPDX-License-Identifier: LGPL-3.0-or-later
// PvZ 无障碍辅助 —— 框架主模块：生命周期、事件接线、动作分发。
// 常规操作动作与键位对齐 PvZA11y；各功能模块只依赖 HostServices 的语义 ABI。
#include <tao/Tao.hpp>
#include "HostServices.hpp"
#include "Speech.hpp"
#include "Settings.hpp"
#include "Narrator.hpp"
#include "BoardControl.hpp"
#include "UiNavigation.hpp"
#include "SeedChooser.hpp"
#include "ImitaterPicker.hpp"
#include "InputMap.hpp"
#include "Award.hpp"
#include "ChallengeNames.hpp"
#include "MainMenu.hpp"
#include "Store.hpp"
#include "HostGuards.hpp"
#include "AbortWatch.hpp"
#include "TraceLog.hpp"
#include "LoggingOptions.hpp"
#include "LoggingPanel.hpp"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
inline bool PlatformIsKeyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
#define PLATFORM_VK_SHIFT   VK_SHIFT
#define PLATFORM_VK_CONTROL VK_CONTROL
#else
inline bool PlatformIsKeyHeld(int) { return false; }
#define PLATFORM_VK_SHIFT   0x10
#define PLATFORM_VK_CONTROL 0x11
#endif

class PvzA11yMod final : public tao::Mod {
public:
    PvzA11yMod() : board(&services), uiNav(&services), chooser(&services), store(&services), imitater(&services) {}
    // Shift 双击窗口：越短双击判定越跟手，单击朗读本身无副作用、立即播报，
    // 窗口只用来识别二连击（二连击内容自带打断）。
    static constexpr auto kDoubleClickWindow = std::chrono::milliseconds(250);

protected:
    tao::Result<void> OnReady() override {
        if (!loggingOptions.Connect(Tao().Host())) LogInfo("日志配置接口不可用，保留默认日志设置。");
        loggingPanel.Connect(
            UI(), loggingOptions,
            [this](std::string text) { Say(std::move(text)); },
            [this] {
                uiNav.SyncFocus(settingsReturnFocus);
                uiNav.RediscoverDialog();
                if (settingsReturnFocus.widget.object_id) uiNav.Focus(settingsReturnFocus, false);
            },
            [this]() -> bool {
                return settings.Data().waveCountdownCue;
            },
            [this](bool enabled) -> bool {
                settings.Data().waveCountdownCue = enabled;
                if (!settings.Save()) LogInfo("未选择用户档案，设置仅本次有效。");
                return true;
            });
        // CRT 崩溃前置钩子（terminate/无效参数/SIGABRT → mods/abort.log 调用栈）：
        // c0000409/5 类 abort 崩溃（如"重新开始游戏"）只能靠它定位，进程内异常
        // 处理器和外部调试器在本宿主均不可行。
        abortwatch::Install();
        if (!services.Connect(Tao().Host())) return TAO_PLUGIN_NOT_AVAILABLE;
        if (!speech.Start()) LogInfo("语音后端不可用；当前仅记录文本。");
        else LogInfo(std::string("语音后端：") + speech.BackendName());
        speech.TakeBackendChange(); // 启动日志已记录初始后端。
        if (!speech.Diagnostics().empty()) LogInfo(speech.Diagnostics());
        if (!speech.Pinned().empty()) LogInfo(std::string("已钉住读屏组件（防卸载崩溃）：") + speech.Pinned());
        settings.SetHooks(
            [this](SettingsData& data) {
                auto profile = State().Profile();
                if (!profile) return false;
                auto& p = profile.Value();
                if (auto v = p.Get<bool>("enabled", data.enabled)) data.enabled = v.Value();
                if (auto v = p.Get<int>("volume", data.volume)) data.volume = std::clamp(v.Value(), 0, 100);
                if (auto v = p.Get<int>("rate", data.rate)) data.rate = std::clamp(v.Value(), -10, 10);
                if (auto v = p.Get<double>("cues", data.cues)) data.cues = std::clamp(v.Value(), 0.0, 1.0);
                if (auto v = p.Get<bool>("zombie_entry_cue", data.zombieEntryCue)) data.zombieEntryCue = v.Value();
                if (auto v = p.Get<bool>("fast_zombie_cue", data.fastZombieCue)) data.fastZombieCue = v.Value();
                if (auto v = p.Get<bool>("gargantuar_hammer_cue", data.gargantuarHammerCue)) data.gargantuarHammerCue = v.Value();
                if (auto v = p.Get<bool>("plant_ready_cue", data.plantReadyCue)) data.plantReadyCue = v.Value();
                if (auto v = p.Get<bool>("wave_countdown_cue", data.waveCountdownCue)) data.waveCountdownCue = v.Value();
                if (auto v = p.Get<int>("wave_countdown_seconds", data.waveCountdownSeconds)) data.waveCountdownSeconds = std::clamp(v.Value(), 1, 15);
                if (auto v = p.Get<int>("store_bounds_x", data.storeX)) data.storeX = v.Value();
                if (auto v = p.Get<int>("store_bounds_y", data.storeY)) data.storeY = v.Value();
                if (auto v = p.Get<int>("store_bounds_w", data.storeW)) data.storeW = v.Value();
                if (auto v = p.Get<int>("store_bounds_h", data.storeH)) data.storeH = v.Value();
                if (auto v = p.Get<int>("grid_rows_adv1", data.gridRowsAdv1)) data.gridRowsAdv1 = v.Value();
                if (auto v = p.Get<int>("grid_rows_adv2", data.gridRowsAdv2)) data.gridRowsAdv2 = v.Value();
                if (auto v = p.Get<int>("grid_rows_adv3", data.gridRowsAdv3)) data.gridRowsAdv3 = v.Value();
                return true;
            },
            [this](const SettingsData& data) {
                auto profile = State().Profile();
                if (!profile) return false;
                auto result = profile.Value().Apply({
                    tao::state::Mutation::Set("enabled", data.enabled),
                    tao::state::Mutation::Set("volume", int64_t(data.volume)),
                    tao::state::Mutation::Set("rate", int64_t(data.rate)),
                    tao::state::Mutation::Set("cues", data.cues),
                    tao::state::Mutation::Set("zombie_entry_cue", data.zombieEntryCue),
                    tao::state::Mutation::Set("fast_zombie_cue", data.fastZombieCue),
                    tao::state::Mutation::Set("gargantuar_hammer_cue", data.gargantuarHammerCue),
                    tao::state::Mutation::Set("plant_ready_cue", data.plantReadyCue),
                    tao::state::Mutation::Set("wave_countdown_cue", data.waveCountdownCue),
                    tao::state::Mutation::Set("wave_countdown_seconds", int64_t(data.waveCountdownSeconds))});
                if (data.storeW > 0) {
                    profile.Value().Apply({
                        tao::state::Mutation::Set("store_bounds_x", int64_t(data.storeX)),
                        tao::state::Mutation::Set("store_bounds_y", int64_t(data.storeY)),
                        tao::state::Mutation::Set("store_bounds_w", int64_t(data.storeW)),
                        tao::state::Mutation::Set("store_bounds_h", int64_t(data.storeH))});
                }
                profile.Value().Apply({
                    tao::state::Mutation::Set("grid_rows_adv1", int64_t(data.gridRowsAdv1)),
                    tao::state::Mutation::Set("grid_rows_adv2", int64_t(data.gridRowsAdv2)),
                    tao::state::Mutation::Set("grid_rows_adv3", int64_t(data.gridRowsAdv3))});
                return result && profile.Value().Flush();
            });
        board.SetSay([this](std::string text, bool interrupt) { Say(std::move(text), interrupt); });
        board.SetObjects(&Objects());
        board.SetLog([this](std::string message) { LogInfo(std::move(message)); });
        // 冒险 1-1..1-3 可玩行数记忆：选卡阶段的小推车权威探测结果跨会话
        // 持久化（周目相关，关卡号无法区分），续玩存档小推车不全时回读。
        board.SetGridMemory(
            [this](int level) {
                const auto& data = settings.Data();
                return level == 1 ? data.gridRowsAdv1 : level == 2 ? data.gridRowsAdv2 : data.gridRowsAdv3;
            },
            [this](int level, int rows) {
                auto& data = settings.Data();
                int& slot = level == 1 ? data.gridRowsAdv1 : level == 2 ? data.gridRowsAdv2 : data.gridRowsAdv3;
                if (slot == rows) return;
                slot = rows;
                if (!settings.Save()) LogInfo("草坪行数探测结果未能写入档案（未选用户？），仅本次会话有效。");
            });
        uiNav.SetSay([this](std::string text, bool interrupt) { Say(std::move(text), interrupt); });
        uiNav.SetOnFocus([this] { board.SetBoardFocus(false); chooser.NoteFocus(uiNav.Focused()); });
        mainMenu.SetSay([this](std::string text, bool interrupt) { Say(std::move(text), interrupt); });
        uiNav.SetMainMenuName([this](const TaoPluginAccessibleInfoV2& info) { return mainMenu.LookupName(info); });
        uiNav.SetSpeechDetails([this](const std::string& name, const TaoPluginAccessibleInfoV2& info) {
            return mainMenu.Details(name, info);
        });
        uiNav.SetLog([this](const std::string& message) { LogInfo(message); });
        uiNav.SetSeedInfo([this](uint64_t item, std::string& prefix, std::string& suffix) {
            return board.SeedCardInfo(item, prefix, suffix);
        });
        chooser.SetSay([this](std::string text, bool interrupt) { Say(std::move(text), interrupt); });
        chooser.SetUi(&uiNav);
        imitater.SetSay([this](std::string text, bool interrupt) { Say(std::move(text), interrupt); });
        imitater.SetLog([this](const std::string& message) { LogInfo(message); });
        imitater.SetSeedInfo([this](uint64_t item, std::string& prefix, std::string& suffix) {
            return board.SeedCardInfo(item, prefix, suffix);
        });
        award.SetSay([this](std::string text, bool interrupt) { Say(std::move(text), interrupt); });
        award.SetLog([this](const std::string& message) { LogInfo(message); });
        if (award.Install(Tao().Host())) LogInfo("奖励窗口朗读已启用。");
        else LogInfo("奖励窗口 Hook 不可用，跳过奖励朗读。");
        challenges.SetLog([this](const std::string& message) { LogInfo(message); });
        uiNav.SetChallengeName([this](int scene, uint32_t role, TaoPluginRect bounds) {
            return challenges.Lookup(scene, role, bounds);
        });
        if (challenges.Install(Tao().Host())) LogInfo("挑战选择界面关卡命名已启用。");
        else LogInfo("挑战选择界面 Hook 不可用，关卡按钮保持未命名。");
        store.SetSay([this](std::string text, bool interrupt) { Say(std::move(text), interrupt); });
        store.SetLog([this](const std::string& message) { LogInfo(message); });
        // 宿主崩溃守卫：订阅宿主观测期间的必崩点拦截（SDK-feedback.md 缺陷 D/E）。
        // 图鉴：DoAlmanacDialog 跳过；僵尸包/特殊包：GetNameString 安全命名
        //（鼠标点击与宿主自身观测两条路径都覆盖）。
        guards.SetSay([this](std::string text, bool interrupt) { Say(std::move(text), interrupt); });
        guards.SetLog([this](const std::string& message) { LogInfo(message); });
        guards.SetNamer([this](int type) -> std::string {
            if (type >= 60 && type < 100) return board.ZombiePacketName(type);
            // 54/55 宝石迷阵洗牌/填坑、58/59 水族馆潜水僵尸/奖杯取具体名；
            // 56/57 老虎机专用包报"特殊卡片"。
            if (type >= 53) return board.SpecialBankPacketName(type);
            return "未知卡片";
        });
        if (guards.Install(Tao().Host())) LogInfo("宿主崩溃守卫已启用。");
        else LogInfo("宿主崩溃守卫部分或全部不可用，详见上方日志。");
        // 商店商品：名称后补价格与"金币不足"；翻页/返回按钮与主菜单商店入口补充命名。
        uiNav.SetItemSuffix([this](const TaoPluginAccessibleNodeV2& node, const TaoPluginAccessibleInfoV2& info) {
            return store.DescribeItem(node, info, board.Context().money);
        });
        uiNav.SetButtonName([this](const TaoPluginAccessibleInfoV2& info) {
            std::string name = store.LookupButtonName(info);
            if (name.empty()) name = store.LookupMenuButton(info);
            // 选卡界面的模仿者按钮：宿主按 GameButton 暴露（0x100000000+mId）但
            // mLabel 为空，是 scene=2 里唯一无名的 50x70 按钮（种子卡是 role=ITEM，
            // 不会误伤；2026-10-10 DumpTree 实证 bounds=(464,515,50,70)）。
            if (name.empty() && int(info.scene) == 2 && info.role == TAO_ACCESSIBLE_ROLE_BUTTON &&
                int(info.bounds.width) == 50 && int(info.bounds.height) == 70)
                name = "模仿者";
            return name;
        });
        uiNav.SetBeforeActivate([this](const TaoPluginAccessibleNodeV2& node,
                const TaoPluginAccessibleInfoV2& info, const std::string& name) -> bool {
            store.NoteActivation(node, info, name);
            if (info.role == TAO_ACCESSIBLE_ROLE_ITEM && info.dialog.object_id &&
                name.find("PvZ 无障碍辅助") != std::string::npos) {
                settingsReturnFocus = node;
                loggingPanel.RequestOpen();
                return true;
            }
            // 主菜单商店入口改走 tao.navigation 请求（F7 同款、非阻塞）：语义激活
            // 会进入商店的模态循环直到关店才返回，期间键盘出口全部失效（戴夫
            // 对话门禁 + 游戏不响应原生键盘），盲用户会被困在店里。
            if (name == "商店") { store.OpenStore(); return true; }
            return false;
        });
        if (services.navigation.request) LogInfo("商店入口（tao.navigation）可用。");
        else LogInfo("tao.navigation 不可用：F7 打开商店将只提示不可用。");
        Narrator::Handlers handlers{
            [this](std::string text) {
                board.SetBoardFocus(false);
                if (!Enabled()) return;
                // 弹窗未关闭时不认为已进入底层场景，关闭后再补播
                if (openDialogs > 0) { pendingScene = std::move(text); return; }
                Say(std::move(text), false);
            },
            [this](TaoPluginAccessibleNodeV2 node, std::string text) {
                // 弹窗不在 widgets.root_* 里，只按根控件遍历会漏掉它（只看到底下的
                // 主菜单）。把事件节点设为锚点，控件导航改从弹窗子树开始。
                tracefile::Write("DIALOG_OPEN id=" + std::to_string(node.widget.object_id) +
                    " openDialogs=" + std::to_string(openDialogs));
                uiNav.SetDialogAnchor(node);
                store.SetAnchor(node);
                TaoPluginWidgetInfoV1 info{};
                info.struct_size = sizeof(info);
                if (node.widget.object_id &&
                    services.widgets.describe(services.widgets.host_context, node.widget, &info) == TAO_PLUGIN_OK)
                    LogInfo("弹窗节点 id=" + std::to_string(node.widget.object_id) +
                        " type=[" + CopyView(info.type_name) + "]");
                if (!Enabled()) return;
                // 同一对话框的重复事件（宿主可能对同一对话框送多次标题/正文，
                // 互为前缀），避免重复播报和 openDialogs 计数漂移。
                if (!openDialogText.empty() &&
                    (text.find(openDialogText) == 0 || openDialogText.find(text) == 0)) return;
                ++openDialogs;
                dialogSpeeches.clear();
                openDialogText = text;
                uiNav.SetSkipBoardWidget(board.IsSlotMachineLevel()); // 跳过中毒棋盘控件
                // 商店：不抢宿主焦点（宿主的模态可交互判定以焦点控件为界，自动聚焦会把
                // 整店判为 BLOCKED）。改为只读播报商品，交互走 Store 的兜底导航。
                // 同时清掉残留的 uiNav 焦点（如进店前的主菜单按钮），保证商店浏览期间
                // storeFallback 判定不被旧焦点干扰；店上叠加的对话框（如购买确认框）
                // 走下面的 Next 分支，不受影响。
                // 商店弹窗的原生标题（"储存。"等）不播——AnnounceOpen 的介绍语更完整，
                // 避免两段重复（2026-10-09 实测）。
                if (store.AnchorIsStore()) {
                    uiNav.SyncFocus({});
                    store.AnnounceOpen();
                }
                else if (imitater.Active()) {
                    // 模仿者对话框：整棵子树被宿主判 BLOCKED（actions=0），uiNav 找不到
                    // 可操作节点，改由 ImitaterPicker 自建虚拟焦点导航（详见 ImitaterPicker.hpp）。
                    Say(std::move(text));       // 弹窗正文：打断旧播报
                    imitater.AnnounceEntry();   // 首项：排队，不打断正文
                }
                else {
                    Say(std::move(text));       // 弹窗正文：打断旧播报
                    uiNav.Next(1, true, false); // 首个控件：排队，不打断正文
                }
            },
            [this] {
                tracefile::Write("DIALOG_CLOSE openDialogs=" + std::to_string(openDialogs) + " -> " +
                    std::to_string(openDialogs > 0 ? openDialogs - 1 : 0));
                if (openDialogs > 0) --openDialogs;
                openDialogText.clear();
                dialogSpeeches.clear();
                uiNav.SyncFocus({}); // 弹窗关闭：清除已失效焦点
                board.SetBoardFocus(false);
                store.SetAnchor({});
                if (store.Browsing()) uiNav.SetDialogAnchor(store.Anchor());
                if (openDialogs == 0) { // 全部弹窗关闭才撤销锚点（嵌套弹窗时保留外层）
                    uiNav.SetDialogAnchor(store.Browsing() ? store.Anchor() : TaoPluginAccessibleNodeV2{});
                    // 模仿者对话框关闭（已选/取消）：焦点归还选卡界面的模仿者按钮。
                    const bool imitaterSeen = imitater.ConsumeOpenFlag();
                    const bool chooserActive = imitaterSeen && chooser.Active();
                    if (imitaterSeen)
                        tracefile::Write("DIALOG_CLOSE imitater chooser=" + std::to_string(chooserActive));
                    if (chooserActive) chooser.RefocusImitaterButton();
                    if (!pendingScene.empty()) {
                        if (Enabled()) Say(std::move(pendingScene), false);
                        pendingScene.clear();
                    }
                }
            },
            [this](TaoPluginAccessibleNodeV2 node, std::string text) {
                if (!Enabled()) return;
                if (loggingPanel.Active()) return;
                const auto focusContext = board.Context();
                const bool chooserFocus = focusContext.scene == 2 &&
                    !(focusContext.flags & (uint32_t(TAO_INPUT_MODAL) | TAO_INPUT_TEXT_EDIT));
                if (chooserFocus && chooser.IgnoreHostFocus(node)) return;
                TaoPluginAccessibleInfoV2 editInfo{};
                editInfo.struct_size = sizeof(editInfo);
                if ((board.Context().flags & TAO_INPUT_TEXT_EDIT) &&
                    services.ui.describe(services.ui.host_context, node, &editInfo) == TAO_PLUGIN_OK &&
                    editInfo.role == TAO_ACCESSIBLE_ROLE_EDIT) {
                    if (editInfo.flags & TAO_WIDGET_FOCUSED) uiNav.ObserveEdit(node, editInfo);
                    return;
                }
                if (SameNode(uiNav.Focused(), node)) return;
                // 商店子树的原生焦点事件不迁移焦点也不播报：商店由 mod 自建列表
                // 接管（Store::Arrow/ActivateFocused），宿主自动聚焦店内按钮/戴夫
                // 气泡的 FOCUS 只在进店瞬间出现，播报全是"未命名控件"/气泡原文。
                // 不查 Active()——进店头几帧 Refresh 未跑到，Owns 只看 storeWidget。
                if (store.Owns(node)) return;
                // 弹窗打开期间，焦点文本与正文重复时（如标题是正文首句）不再播报
                if (!openDialogText.empty() && !text.empty() &&
                    openDialogText.find(text) != std::string::npos) return;
                // 事件节点与枚举节点身份可能不一致，按文本去重已播报过的焦点
                if (!text.empty() && std::find(dialogSpeeches.begin(), dialogSpeeches.end(), text) != dialogSpeeches.end()) return;
                // 只有可操作节点才迁移焦点；对话框标题等纯信息节点只朗读
                bool actionable = false;
                TaoPluginAccessibleInfoV2 info{};
                info.struct_size = sizeof(info);
                if (services.ui.describe(services.ui.host_context, node, &info) == TAO_PLUGIN_OK)
                    actionable = (info.actions & (uint32_t(TAO_ACCESSIBLE_ACTION_ACTIVATE) |
                        TAO_ACCESSIBLE_ACTION_SET_VALUE | TAO_ACCESSIBLE_ACTION_SET_TEXT | TAO_ACCESSIBLE_ACTION_FOCUS)) != 0;
                if (chooserFocus && SeedChooser::IsCardItem(node.item) && (info.flags & TAO_WIDGET_FOCUSED))
                    actionable = true; // 飞行期 actions=0，卡片仍是逻辑导航起点。
                if (actionable) {
                    if (!(info.flags & TAO_WIDGET_FOCUSED)) return;
                    if (info.scene == 1 && !info.dialog.object_id) {
                        const auto name = uiNav.SpokenName(node, info);
                        if (!name.empty()) text = name;
                    }
                    uiNav.SyncFocus(node);
                    if (chooserFocus) chooser.ObserveHostFocus(node);
                    board.SetBoardFocus(false);
                }
                Say(std::move(text), false);
            },
            [this](std::string text) { if (Enabled()) Say(std::move(text), false); },
            [this](uint32_t) { board.Context(); board.SetBoardFocus(false); if (Enabled()) Say("部分提示已合并", false); },
        };
        narrator.SetHandlers(handlers);
        settings.Load();
        ApplySettings();
        if (!narrator.Subscribe(services.a11y)) return TAO_PLUGIN_NOT_AVAILABLE;
        TaoPluginInputEventRegistrationV1 registration{};
        registration.struct_size = sizeof(registration);
        registration.priority = 100;
            registration.kind_mask = (UINT64_C(1) << TAO_INPUT_EVENT_KEY_DOWN) | (UINT64_C(1) << TAO_INPUT_EVENT_GAMEPAD_BUTTON_DOWN);
        registration.callback = &PvzA11yMod::OnInputEvent;
        registration.callback_context = this;
        const auto status = services.input.subscribe(services.input.host_context, &registration, &keys);
        if (status != TAO_PLUGIN_OK) {
            narrator.Unsubscribe();
            return status;
        }
        if (Enabled()) Say("PvZ 无障碍辅助已启动。F9 帮助，F8 关闭。");
        return TAO_PLUGIN_OK;
    }
    void OnUpdate(uint64_t tick) override {
        // 普通帧及阻塞型 Mod 管理器的嵌套帧都可操作设置。
        try { loggingPanel.Pump(); }
        catch (const std::exception& error) { Say(std::string("设置界面操作失败：") + error.what()); }
        // 正常帧和嵌套模态帧都刷新，节流且不写盘，配置修改后约一秒生效。
        if (tick >= nextLogConfigPoll) {
            nextLogConfigPoll = tick + 100;
            loggingOptions.Refresh();
        }
        // 可重入闩锁：ui.invoke(ACTIVATE) 执行期间宿主会嵌套泵 OnUpdate（2026-10-09
        // trace 实证：未解锁模式按钮的 invoke 栈内投递 DIALOG_OPEN 并分发了队列中
        // 的下一次回车，两个激活在栈上重叠，宿主拆帧崩溃）。嵌套帧只泵无障碍事件
        // 与语音（弹窗内容照常朗读，宿主的泵循环也能收敛——完全跳过会饿死它），
        if (inUpdate) {
            abortwatch::g_aliveLastTick = tick;
            Timed(tick, "a11y-pump", [&] { narrator.Pump(); speech.Pump(); });
            if (Enabled()) {
                uiNav.PumpEdit();
                if (editReadPending) { editReadPending = false; uiNav.ReadCurrentEdit(); }
                // 嵌套激活也可能打开更深一层（额外设置 → Mod → 配置），
                // 停订后 DIALOG_OPEN 丢失；外层 invoke 尚未返回，仍须发现顶层弹窗。
                if (resubscribeNeeded && tick % 10 == 0 && !store.Browsing() && !loggingPanel.Active())
                    uiNav.RediscoverDialog();
                if (nestedDumpPending) { nestedDumpPending = false; uiNav.DumpTree(); }
            }
            if (!nestedStoreActions.empty() && !inStoreAction) {
                if (store.Browsing()) {
                    const auto pending = nestedStoreActions.front();
                    nestedStoreActions.erase(nestedStoreActions.begin());
                    struct StoreActionGuard { bool& flag; ~StoreActionGuard() { flag = false; } } storeGuard{inStoreAction = true};
                    Dispatch(pending.action, pending.param);
                } else nestedStoreActions.clear();
                if (resubscribeNeeded && !inStoreAction && RestoreStoreNavigation()) {
                    resubscribeNeeded = false;
                    hostSuppressed = false;
                    narrator.Subscribe(services.a11y);
                    LogInfo("商店购买确认已关闭：恢复商店导航与宿主观测");
                }
            }
            // 阻塞激活期间允许方向键导航（FOCUS invoke 实测安全），否则键盘用户
            // 困死在模态里无法移动焦点。
            if (!modalNavQueue.empty()) {
                const auto nav = modalNavQueue.front();
                modalNavQueue.erase(modalNavQueue.begin());
                if (nav.action == Action::Option) uiNav.Next(nav.param);
                else {
                    const int dx = nav.action == Action::Right ? 1 : nav.action == Action::Left ? -1 : 0;
                    const int dy = nav.action == Action::Down ? 1 : nav.action == Action::Up ? -1 : 0;
                    if (store.Browsing()) store.Arrow(dx, dy);
                    else uiNav.Arrow(dx, dy);
                }
            }
            // 阻塞激活/原生模态循环期间用户按了回车/退格：停订宿主观测后经嵌套
            // ACTIVATE（回车，按焦点按钮）或 CancelDialog（退格）关闭弹窗。
            // 直接嵌套激活曾实测崩溃（2026-10-09，观测管线在拆帧瞬间介入），
            // 停订是"重新开始游戏"同款规避；invoke 返回后由外层帧恢复订阅，
            // 弹窗状态按丢失事件重置。焦点不可激活（如入口按钮在模态中失去
            // 动作）时先导航到首个可操作控件再等下一次回车。
            if (modalDismissPending > 0) {
                --modalDismissPending;
                if (store.Browsing()) {
                    if (nestedStoreActions.size() < 8)
                        nestedStoreActions.push_back({modalDismissAction == 2 ? Action::Deny : Action::Confirm, 0});
                    modalDismissPending = 0;
                } else if (uiNav.FocusedIsActionable()) {
                    tracefile::Write("nested frame: modal dismiss (unsubscribe + activate) @" + std::to_string(tick));
                    narrator.Unsubscribe();
                    resubscribeNeeded = true;
                    if (modalDismissAction == 2) uiNav.CancelDialog();
                    else uiNav.ActivateFocused();
                } else {
                    tracefile::Write("nested frame: focus not actionable, re-navigating @" + std::to_string(tick));
                    uiNav.SyncFocus({});
                    uiNav.Next(1);
                }
            }
            if (nestedTraceCount < 10 || nestedTraceCount % 500 == 0)
                tracefile::Write("OnUpdate: nested pump-only @" + std::to_string(tick));
            ++nestedTraceCount;
            return;
        }
        modalDismissPending = 0; // 正常帧开始：清掉已过期的模态关闭请求
        modalDismissAction = 0;
        modalNavQueue.clear();
        nestedStoreActions.clear();
        if (resubscribeNeeded) { // 阻塞 invoke 已返回 / 停订期间激活弹窗后：恢复宿主观测（关闭事件丢失由既有逻辑重置）
            resubscribeNeeded = false;
            // 置回未停订，让下方 suppress 判定按探针值重估：停订条件仍在（如
            // roll_count 仍>0）时会再次停订，读者窗口只有一到数帧，避开滚动帧即可。
            hostSuppressed = false;
            if (narrator.Subscribe(services.a11y)) {
                LogInfo("停订后宿主观测已恢复");
                if (store.Active()) RestoreStoreNavigation();
                else if (openDialogs > 0 && !(board.Context().flags & TAO_INPUT_MODAL))
                    ResetDialogTracking();
            }
        }
        nestedTraceCount = 0;
        struct UpdateGuard { bool& flag; ~UpdateGuard() { flag = false; } } guard{inUpdate = true};
        abortwatch::g_aliveLastTick = tick;
        abortwatch::g_aliveLastScene = board.Context().scene;
        if (tick % 300 == 0) {
            abortwatch::AppendNote("heartbeat");
            const auto hbCtx = board.Context();
            tracefile::Write("heartbeat tick=" + std::to_string(tick) +
                " scene=" + std::to_string(hbCtx.scene) +
                " modal=" + std::to_string((hbCtx.flags & TAO_INPUT_MODAL) != 0) +
                " openDialogs=" + std::to_string(openDialogs));
        }
        Mark(tick, "S"); // 临时诊断：OnUpdate 入口
        const auto tickStart = std::chrono::steady_clock::now();
        // 宿主对 OnUpdate 一次超预算即永久禁用更新回调（2026-10-08 选卡界面实测：
        // 单 tick 8.7ms 后 "Disabled native update callback"，输入只入队不分发，
        // mod 整体失能且无法恢复）。所有可延迟的周期段先查预算，超了留给下个窗口。
        const auto overBudget = [tickStart](double ms) {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tickStart).count() >= ms;
        };
        if (!settings.Loaded() && tick % 100 == 0) {
            settings.Load();
            ApplySettings();
        }
        // 商店存在性检测与导航按钮命名（节流）。开店提示排队播报，不打断。
        const int refreshScene = board.Context().scene;
        if (tick % 10 == 2 && !overBudget(4.0)) {
            const auto context = board.Context();
            mainMenu.Pump(context.scene, context.flags, Enabled());
        }
        if (tick % 10 == 1 && !overBudget(4.0) &&
            (store.Active() || (board.Context().flags & TAO_INPUT_MODAL) || (refreshScene != 2 && refreshScene != 3))) {
            Timed(tick, "store", [&] { store.Refresh(); });
            Mark(tick, "T"); // 临时诊断：商店刷新之后
            if (store.Active() && store.ConsumeJustOpened() && Enabled()) {
                Say("疯狂戴夫的商店。方向键浏览，F7 朗读本页商品，回车购买，退格退出。", false);
                store.AnnounceItems();
            }
        }
        // 商店入口按钮位置新学到（含键盘进店那次：开店期间嵌套泵帧 Refresh
        // 不运行，出店后首个正常帧在此落盘）→ 写入档案设置，跨会话生效。
        if (store.ConsumeLearnedDirty()) {
            auto& data = settings.Data();
            const auto rect = store.MenuStoreBounds();
            data.storeX = int(rect.x); data.storeY = int(rect.y);
            data.storeW = int(rect.width); data.storeH = int(rect.height);
            if (!settings.Save()) LogInfo("商店入口位置未能写入档案（未选用户？），仅本次会话有效。");
        }
        // 输入回调只入队，动作在这里执行：场景切换等重操作会超出输入回调时间预算，
        // 超时回调会被宿主移除（“失败或超时的回调会被移除，事件默认放行”）。
        // 宿主键码表无映射的 OEM 标点（逗号/句号/减号/等号上报为键码 0）：
        // 用 Win32 物理键态甄别具体键，再走同一套消费/分发规则。
        if (tick % 10 == 3) Timed(tick, "probe", [&] { // 采样老虎机/我是僵尸关卡状态
            const auto ctx = board.Context();
            // 停订期间升起的新弹窗（如"新的游戏"后的询问对话框、空格/ESC 打开的
            // 暂停菜单）收不到 DIALOG_OPEN 事件：锚点停在已拆除的旧弹窗上、
            // Nodes() 永远为空且标题不播报。中心命中测试发现顶层弹窗即换锚补播
            //（2026-10-09 实测询问对话框卡住 3 分钟，靠未映射空格漏进原生才推进）。
            if (hostSuppressed && (ctx.flags & TAO_INPUT_MODAL)) uiNav.RediscoverDialog();
            slotProbe = board.IsSlotMachineLevel();
            izombieProbe = board.IsIZombieLevel();
            // 老虎机与我是僵尸都可能以"读档续玩"进关：控件树里的 Board 控件
            // 被 ui.describe 会崩，导航枚举时整棵跳过（识别见 UiNavigation）。
            uiNav.SetSkipBoardWidget(slotProbe || izombieProbe);
            const int rollCount = slotProbe ? board.SlotMachineRollCount() : -1;
            slotPulled = rollCount > 0;
            if (slotProbe) {
                // 拉杆/滚动状态变化 → 暂缓自动收集一个窗口（避开拉杆后枚举崩溃）
                if (rollCount >= 0 && lastRollCount >= 0 && rollCount != lastRollCount)
                    slotSpinHoldUntil = tick + kSlotSpinHold;
                lastRollCount = rollCount;
            } else {
                lastRollCount = -1;
                slotSpinHoldUntil = 0;
            }
            const bool modal = slotProbe && (ctx.flags & TAO_INPUT_MODAL) != 0;
            const bool resumed = slotProbe && !modal && slotModalSeen; // 从弹窗返回游戏
            // 停订宿主观测期间收不到弹窗事件：自查模态上升沿补播标题（含暂停菜单）
            if (modal && !slotModalSeen) {
                slotModalSeen = true;
                if (hostSuppressed) {
                    const std::string title = CopyView(ctx.title);
                    Say(title.empty() ? std::string("对话框") : title, false);
                }
            } else if (!modal) {
                slotModalSeen = false;
            }
            // 旋转结束（底部建议出现）或从弹窗返回时，补播一次本次结果
            if (slotProbe) {
                const bool adviceShown = board.SlotAdviceVisible();
                if ((adviceShown && !slotAdviceShown) || resumed) {
                    const std::string result = board.SlotResultText();
                    if (!result.empty() && (resumed || result != lastSlotResult)) {
                        lastSlotResult = result;
                        Say(result, false);
                    }
                }
                slotAdviceShown = adviceShown;
            } else {
                slotAdviceShown = false;
                lastSlotResult.clear();
            }
        });
        // 老虎机关卡"已拉杆后"停订宿主观测：只要存在订阅读者，宿主就每 tick 全量计算
        // board->mAdvice，拉杆/滚动帧解引用悬垂指针崩溃（0xC0000005，读者归零是唯一
        // 规避手段）。实测拉杆前订阅正常（暂停菜单可播报），但一旦拉过杆，旋转期间即使
        // 人在暂停菜单里恢复订阅也会崩，故以 roll_count>0 为界整段停订至离开关卡。
        // 导航光标（uiNav）走 widgets/ui 接口，不受订阅影响。
        // 我是僵尸（2026-10-08）：棋盘场景存在宿主观测崩溃触发点——scene=2 备战期
        // 订阅保持数百 tick 无恙，关卡启动/建议重算瞬间崩在 tick 之间（OnUpdate 的
        // S..E 标记全部走完），属宿主自身管线，同老虎机 mAdvice 家族。与老虎机的
        // 区别：入关的 继续游戏？/新游戏？ 对话框靠订阅事件播报与导航，且实测弹窗
        // 期间订阅 2000+ tick 无恙，故只在"无弹窗"时停订；弹窗上升沿恢复订阅并
        // 补播标题（停订期间收不到事件）。
        const bool izModal = izombieProbe && (board.Context().flags & TAO_INPUT_MODAL) != 0;
        const bool suppress = (slotProbe && slotPulled) || (izombieProbe && !izModal);
        if (suppress != hostSuppressed) {
            hostSuppressed = suppress;
            if (suppress) {
                narrator.Unsubscribe();
                LogInfo(slotProbe ? "老虎机已拉杆：停订宿主观测（规避崩溃），离开关卡后恢复"
                                     : "我是僵尸：停订宿主观测（规避崩溃），弹窗期间恢复");
            } else if (narrator.Subscribe(services.a11y)) {
                if (izModal) { // 停订期间弹窗升起，事件已错过，恢复后补播标题
                    const std::string title = CopyView(board.Context().title);
                    Say(title.empty() ? std::string("对话框") : title, false);
                }
                LogInfo("宿主观测播报已恢复");
                // 停订期间弹窗关闭事件丢失 → 计数/锚点泄漏，按游戏模态态对齐。
                if (openDialogs > 0 && !(board.Context().flags & TAO_INPUT_MODAL)) {
                    LogInfo("检测到弹窗关闭事件丢失（无模态弹窗但计数未清零），重置弹窗状态");
                    ResetDialogTracking();
                }
            }
        }
        if (unknownKeyPending) {
            unknownKeyPending = false;
            PollOemPunctuation();
        }
        // 先泵无障碍事件再分发动作：激活弹窗按钮（确定/返回）后弹窗关闭事件是
        // 异步送达的，先泵可保证下一次回车分发前焦点已被关闭事件清空，否则
        // 会对着正在拆除的弹窗控件二次 invoke（宿主二次 ButtonPress 崩溃，
        // 2026-10-09 生存模式未解锁对话框实测）。
        board.NoteTick(tick); // ReadCell 路径的提示音（宝石迷阵匹配哔声）无 tick 上下文
        Timed(tick, "a11y-pump", [&] { narrator.Pump(); });
        if (Enabled() && !overBudget(4.0)) uiNav.PumpEdit();
        if (editReadPending) { editReadPending = false; if (Enabled()) uiNav.ReadCurrentEdit(); }
        if (!pendingActions.empty()) {
            // 每 tick 最多分发 2 个动作：选卡方向键一次 Scan 是全树遍历，连发突发
            // 会叠爆 OnUpdate 预算（宿主一次超时即永久禁用回调）。剩余动作下帧继续。
            const size_t count = std::min<size_t>(pendingActions.size(), 2);
            for (size_t i = 0; i < count; ++i) {
                const auto& pendingAction = pendingActions[i];
                Mark(tick, ("D" + std::to_string(int(pendingAction.action))).c_str()); // 临时诊断：动作分发之前
                Dispatch(pendingAction.action, pendingAction.param);
            }
            pendingActions.erase(pendingActions.begin(), pendingActions.begin() + count);
        }
        Mark(tick, "K"); // 临时诊断：按键分发之后
        if (!debugKeys.empty()) { // 临时调试：按键事件落点追踪
            std::string line = "键码追踪";
            for (const int code : debugKeys)
                line += code < 0 ? " 未消费" + std::to_string(-code) : " 未映射" + std::to_string(code);
            debugKeys.clear();
            LogInfo(std::move(line));
        }
        // 关卡末尾奖励自动收集（节流）：只领取通关种子包/奖杯/铲子等流程奖励物。
        // 老虎机关卡照常收集（否则通关奖励不领），但避开拉杆后的滚动窗口。
        // 宝石迷阵/旋风/水族馆同周期静默收集场上阳光金币（这些关卡无向日葵，
        // 阳光全靠拾取；水族馆阳光落不进网格，手动收集对视障用户不可行）。
        Mark(tick, "CA"); // 临时诊断：自动收集之前
        if (Enabled() && tick % 30 == 15 && (!slotProbe || tick >= slotSpinHoldUntil) && !overBudget(4.0))
            Timed(tick, "collect", [&] { board.AutoCollectRewards(); board.AutoCollectSunCoins(); });
        Mark(tick, "CB"); // 临时诊断：自动收集之后
        // 僵尸波次 / 快速僵尸提示音（扫描在 BoardControl 内节流到 100ms）
        const auto& cueData = settings.Data();
        Mark(tick, "UA"); // 临时诊断：提示音更新之前
        if (Enabled() && cueData.cues > 0 &&
            (cueData.zombieEntryCue || cueData.fastZombieCue || cueData.gargantuarHammerCue || cueData.plantReadyCue || cueData.waveCountdownCue)) {
            const auto context = board.Context();
            if (board.Gameplay(context))
                board.UpdateCues(context, tick, cueData.zombieEntryCue, cueData.fastZombieCue, cueData.plantReadyCue,
                                cueData.waveCountdownCue, cueData.waveCountdownSeconds, cueData.gargantuarHammerCue);
            else board.ResetCues();
        } else board.ResetCues();
        Mark(tick, "UB"); // 临时诊断：提示音更新之后
        // 拆段计时定位慢源；chooser.Pump 是全树扫描（选卡驻留重试），超预算留到
        // 下帧（refocusTicks 按帧计，跳帧只是重试变稀，不会提前超时）。
        Timed(tick, "narrator", [&] {
            speech.Pump();
            const auto backend = speech.TakeBackendChange();
            if (!backend.empty()) LogInfo(std::string("语音后端切换：") + backend);
            award.Pump();
        });
        if (!overBudget(4.0)) Timed(tick, "chooser", [&] { chooser.Pump(); });
        Mark(tick, "E"); // 临时诊断：OnUpdate 结束
    }
    tao::Result<void> OnStopping() override {
        if (keys) services.input.unsubscribe(services.input.host_context, keys);
        keys = 0;
        narrator.Unsubscribe();
        board.ReleaseFreeze();
        speech.Close();
        return TAO_PLUGIN_OK;
    }

private:
    void LogInfo(std::string text) const {
        if (loggingOptions.ModEnabled()) Log().Info(text);
    }
    bool Enabled() { return settings.Data().enabled; }
    // 慢段诊断：OnUpdate 超 5ms 的段落记日志（宿主对超时会永久禁用更新回调，
    // 需要定位是哪段耗时）。常态下零输出。
    template <typename F>
    void Timed(uint64_t tick, const char* tag, F&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms > 5.0) LogInfo(std::string("OnUpdate 慢段 ") + tag + " @tick " +
            std::to_string(tick) + "：" + std::to_string(int(ms)) + "ms");
    }
    void Mark(uint64_t tick, const char* tag) { // 临时诊断：定位崩溃点（仅我是僵尸关卡输出，避免刷屏）
        if (Enabled() && board.IsIZombieLevel()) LogInfo(std::string("·") + tag + "@" + std::to_string(tick));
    }
    void Say(std::string text, bool interrupt = true) {
        if (text.empty()) return;
        lastSpoken = text;
        if (!openDialogText.empty() && dialogSpeeches.size() < 16) dialogSpeeches.push_back(text);
        LogInfo(text);
        speech.Say(std::move(text), interrupt);
    }
    void ApplySettings() {
        const auto& data = settings.Data();
        speech.Settings(data.volume, data.rate);
        services.audio.set_volume(services.audio.host_context, data.cues);
        // 持久化的商店入口按钮位置（动态学习成果）：w>0 才覆盖，避免空设置
        // 擦掉本会话刚学到的值。
        if (data.storeW > 0)
            store.SetMenuStoreBounds(TaoPluginRect{data.storeX, data.storeY, data.storeW, data.storeH});
    }
    void Help() {
        Say("方向键移动，回车确认、种植或锤击，退格放回手上的卡片或工具，反引号选择铲子、再按切换南瓜层。"
            "数字键选数字卡槽（种子银行在前，场上掉落的植物卡在后；我是僵尸选僵尸包），减号等号循环选卡。老虎机用 S 拉杆；"
            "砸花瓶关卡光标为空且该格有花瓶时回车砸罐。"
            "空手在玉米炮上回车进入瞄准，移动到目标格再回车发射，退格取消瞄准；C 键自动选择就绪炮轰击当前格。"
            "宝石迷阵按住 Ctrl 加方向键拖动植物凑三消，旋风回车旋转当前格，僵尸水族馆回车投喂鱼食，"
            "这几关数字键单击朗读特殊操作（重新洗牌、填平弹坑、升级植物、买潜水僵尸、领奖杯）、双击执行，"
            "阳光金币自动收集，浏览到有匹配的格子会哔声提示。"
            "Tab 冻结游戏，N 冻结时推进一帧。谁笑到最后开局前 Tab 展开攻击。"
            "禅境花园数字键选工具（1 浇水壶 2 肥料 3 杀虫剂 4 留声机 5 巧克力 6 手套 7 卖植物 8 手推车，0 前往下一个花园），"
            "回车对当前格植物使用或放下搬移的植物，退格放回工具、空手时返回主菜单，Tab 打开商店，Shift 播报花园总览。"
            "空手回车叫醒睡觉的臭臭，巧克力对空格回车喂臭臭。"
            "智慧树回车拿起肥料、再按回车施肥，退格放回肥料或返回花园，Tab 或 F7 打开商店，Shift 朗读树高。"
            "F1 重读，F2 格子信息，F3 阳光金币，F4 关卡进度，F6 切换僵尸简称，逗号句号循环查看僵尸。"
            "主菜单 F2 朗读欢迎语、当前存档和冒险进度，F4 重读冒险关卡。"
            "Shift 播报本行僵尸，双击 Shift 播报本行小推车，选卡时 Shift 朗读本关出怪、双击朗读场景。"
            "F7 打开疯狂戴夫的商店，店内方向键浏览、回车购买、退格退出。"
            "Esc 暂停菜单。F8 关闭无障碍。", false);
    }
    // 只做便宜的判断，供输入回调决定本次事件是否消费；与实际执行共用同一套规则。
    bool ShouldConsume(Action action) {
        if (action == Action::ToggleMod) return true;
        if (!Enabled()) return false;
        if (action == Action::Help) return true;
        if (action == Action::Store) return true; // F7：打开/重读商店
        if (action == Action::DumpTree) return true; // 临时调试
        if (award.Active()) {
            switch (action) {
            case Action::Up:
            case Action::Down:
            case Action::Left:
            case Action::Right:
            case Action::Confirm:
            case Action::Deny:
                return false; // 奖励窗口：按键交还原生处理（回车/空格推进）
            default:
                break;
            }
        }
        const auto context = board.Context();
        if (context.flags & TAO_INPUT_TEXT_EDIT) return action == Action::Info1;
        switch (action) {
        case Action::Start:
            // 棋盘内由 mod 显式打开暂停菜单（ESC 原生处理在棋盘内不可靠，
            // 实测事件放行后宿主不响应）；界面/弹窗仍交原生。
            return board.Gameplay(context);
        case Action::Up:
        case Action::Down:
        case Action::Left:
        case Action::Right:
        case Action::Confirm:
        case Action::Deny:
        case Action::Option:
        case Action::CycleLeft:
        case Action::CycleRight:
        case Action::Info1:
        case Action::ZombieFilter:
            return true;
        case Action::Info2:
        case Action::Info4:
            return (context.scene == 1 && !(context.flags & TAO_INPUT_MODAL)) || board.Gameplay(context);
        case Action::RowInfo:
            // 战斗/铲子教学中播本行僵尸；选卡界面（scene=2）朗读本关出怪。
            return board.Gameplay(context) ||
                context.scene == int(tao::enums::GameScenes::SCENE_LEVEL_INTRO.Value());
        default:
            break;
        }
        return board.Gameplay(context);
    }
    bool Dispatch(Action action, int param) {
        auto& data = settings.Data();
        // Shift 后接其他键（如 Shift+Tab 反向切换）时重置双击窗口，避免把
        // 间隔较近的两次操作误判成双击。
        if (action != Action::RowInfo) shiftPending = false;
        if (action == Action::ToggleMod) {
            data.enabled = !data.enabled;
            uiNav.ResetEdit();
            if (!data.enabled) board.ReleaseFreeze();
            ApplySettings();
            if (!settings.Save()) LogInfo("未选择用户档案，设置仅本次有效。");
            Say(data.enabled ? "无障碍已开启" : "无障碍已关闭");
            return true;
        }
        if (!data.enabled) return false;
        const auto context = board.Context();
        if (action == Action::Help) {
            Help();
            return true;
        }
        if (action == Action::DumpTree) { // 临时调试
            uiNav.DumpTree();
            return true;
        }
        if (action == Action::Store) { // F7：在商店内逐项播报本页商品，禅境花园内开店，否则请求打开商店
            if (store.Active()) store.AnnounceItems();
            else if (board.IsZenGarden() && board.Gameplay(context) && !(context.flags & TAO_INPUT_MODAL))
                board.OpenGardenStore();
            else if (board.IsTreeOfWisdom() && board.Gameplay(context) && !(context.flags & TAO_INPUT_MODAL))
                board.OpenTreeStore();
            else if (board.Gameplay(context)) Say("请在主菜单打开商店");
            else store.OpenStore();
            return true;
        }
        if (context.flags & TAO_INPUT_TEXT_EDIT) {
            if (action == Action::Info1) return uiNav.ReadCurrentEdit();
            return false;
        }
        const bool gameplay = board.Gameplay(context);
        // 选卡界面（非模态）：由 SeedChooser 策略接管方向键/回车/加减号，
        // 钉住卡片网格顺序，避免选中后焦点跳到已选行。
        const bool choosing = !gameplay && !(context.flags & TAO_INPUT_MODAL) && chooser.Active();
        // 模仿者对话框（模态，仅在选卡场景出现）：整棵子树被宿主 BLOCKED，
        // 语义导航不可用，由 ImitaterPicker 自建导航。检测要枚举对话框子项，
        // 限定 scene=2 模态，避免暂停菜单/商店里每次按键都白扫一遍。
        const bool imitaterActive = !gameplay && context.scene == 2 &&
            (context.flags & TAO_INPUT_MODAL) && imitater.Active();
        // 商店：始终走 mod 侧自建商品列表（方向键浏览、回车购买），不调用
        // uiNav.Focus——宿主的模态可交互判定以焦点控件为界，抢焦点会把整店判为 BLOCKED。
        // 购买确认框（[BUY_ITEM] 是/否）打开时其按钮由 uiNav 聚焦（DIALOG_OPEN 里
        // uiNav.Next），方向键/回车改走控件导航；确认框关闭时 DIALOG_CLOSE 清焦点，
        // 自动回到商店浏览。开店瞬间 uiNav 焦点已被清空（见 DIALOG_OPEN 处理）；
        // 键盘进店时宿主会对店内原生按钮（主菜单/翻页）自动发 FOCUS 重建焦点，
        // 焦点落在商店子树内视同无焦点（Owns 判定），否则方向键被 uiNav 抢去
        // 只能在未命名翻页按钮间打转（2026-10-09 实测，鼠标进店无此问题）。
        const bool storeFallback = !gameplay && !choosing && store.Browsing() &&
            (!uiNav.HasFocus() || store.Owns(uiNav.Focused()));
        switch (action) {
        case Action::Up:
        case Action::Down:
        case Action::Left:
        case Action::Right: {
            const int dx = action == Action::Right ? 1 : action == Action::Left ? -1 : 0;
            const int dy = action == Action::Down ? 1 : action == Action::Up ? -1 : 0;
            if (gameplay) {
                // 智慧树：没有格子可导航，方向键朗读树高与操作提示。
                if (board.IsTreeOfWisdom()) board.AnnounceTreeStatus();
                // 宝石迷阵：Ctrl+方向键 = 把当前格植物拖向该方向凑三消；
                // 不带修饰键时照常移动光标朗读（KeyCode 17 = VK_CONTROL，
                // 与 Option 分支 Shift 的 KeyCode(16) 同款判定）。
                else if (board.IsBeghouledLevel() && Input().Down(tao::KeyCode(17)))
                    board.DragPlant(dx, dy);
                else {
                    board.MoveCursor(dx, dy);
                    board.ReadCell();
                }
            } else if (choosing) {
                chooser.Arrow(dx, dy);
            } else if (imitaterActive) {
                imitater.Arrow(dx, dy);
            } else if (storeFallback) {
                store.Arrow(dx, dy);
            } else {
                uiNav.Arrow(dx, dy);
            }
            return true;
        }
        case Action::Confirm:
            // 打僵尸关卡回车始终优先落棋盘：进关/选卡失败时棋盘焦点未建立，
            // 此时 UI 焦点可能停在"菜单"按钮上，直接激活会打开暂停菜单；
            // 对话框由 TAO_INPUT_MODAL 另行转给控件导航，不受影响。
            tracefile::Write("Dispatch Confirm: gameplay=" + std::to_string(gameplay) +
                " choosing=" + std::to_string(choosing) + " storeFallback=" + std::to_string(storeFallback) +
                " uiFocus=" + std::to_string(uiNav.HasFocus()) +
                " modal=" + std::to_string((context.flags & TAO_INPUT_MODAL) != 0) +
                " openDialogs=" + std::to_string(openDialogs));
            if (gameplay && (board.HasBoardFocus() || context.cursor == tao::enums::CursorType::CURSOR_TYPE_COBCANNON_TARGET.Value() || board.IsWhackLevel() || board.IsVaseLevel() || board.IsZenGarden() || board.IsTreeOfWisdom() || board.HasSpecialBank())) {
                // 智慧树优先：回车 = 空手拿起肥料/持肥对树施肥。
                if (board.IsTreeOfWisdom()) board.UseTreeFood();
                // 禅境花园优先：回车 = 使用手上的工具/放下搬移中的植物
                //（含铲子光标——花园禁铲，避免误铲玩家收集的盆栽）。
                else if (board.IsZenGarden()) board.UseGardenTool();
                // 特殊关卡：旋风回车旋转当前格，水族馆回车喂食，经典宝石迷阵
                // 回车只给拖动提示（交换走 Ctrl+方向键）。
                else if (board.IsBeghouledTwistLevel()) board.RotateCurrent();
                else if (board.IsZombiquariumLevel()) board.FeedCurrent();
                else if (board.IsBeghouledLevel()) board.BeghouledEnterHint();
                else if (context.cursor == tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value()) board.ShovelCurrent();
                else if (board.IsWhackLevel() &&
                    (context.cursor == tao::enums::CursorType::CURSOR_TYPE_HAMMER.Value() ||
                     context.cursor == tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value())) board.WhackCurrent();
                else if (board.IsVaseLevel() &&
                    context.cursor == tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value() &&
                    board.VaseAtCursor().object_id) board.BreakVase();
                else if (board.TryCannonConfirm()) { }
                else board.Invoke(TAO_BOARD_PLANT);
            } else if (choosing) {
                chooser.ActivateFocused();
            } else if (imitaterActive) {
                imitater.Activate();
            } else if (storeFallback) {
                store.ActivateFocused();
            } else if (uiNav.HasFocus()) {
                // 停订宿主观测期间激活弹窗按钮（如老虎机"新的游戏"）：弹窗关闭事件
                // 已丢失，且激活可能触发较慢的游戏切换（拆板重建实测约 300 tick），
                // 期间界面无控件、mod 失聪——先播报提示让用户知道等待是正常的
                //（2026-10-09 复现 2：用户在切换窗口内误判卡死退出）。
                const bool activated = uiNav.ActivateFocused();
                if (activated && hostSuppressed)
                    Say("已激活，请稍候", false); // 后续可能是慢切换（拆板重建）或新询问对话框
                // 走"重新开始"同款恢复：下一帧强制重订阅并重估停订条件
                //（roll_count 仍>0 会自动再次停订；新棋盘就位后即恢复播报）。
                if (hostSuppressed) resubscribeNeeded = true;
            } else {
                uiNav.Next(1);
            }
            return true;
        case Action::Deny:
            if (gameplay) board.ReturnHeldItem(); // 退格：放回手上的卡片或工具
            // 模仿者对话框：原生关闭（KillDialog），关闭事件到达后焦点归还模仿者按钮。
            else if (imitaterActive) imitater.Cancel();
            // 商店浏览期间 mod 不持有 uiNav 焦点（或焦点就在店内控件上）：
            // CancelDialog 取不到 info.dialog 会静默失败，改走 Store 专用退店。
            else if (storeFallback)
                store.Cancel();
            else {
                uiNav.CancelDialog();
                // 停订期间退格取消弹窗同样丢失关闭事件，与 Confirm 激活同款恢复。
                if (hostSuppressed) resubscribeNeeded = true;
            }
            return true;
        case Action::Start:
            if (gameplay) board.OpenPauseMenu();
            return gameplay; // 界面/弹窗：ESC 放行给原生
        case Action::Option:
            if (storeFallback) { store.NextButton(Input().Down(tao::KeyCode(16)) ? -1 : 1); return true; }
            // 禅境花园：Tab 打开疯狂戴夫的商店（花园商店按钮的原生终点）；
            // 花园里冻结没意义。模态中（店内/暂停菜单）仍走控件导航。
            if (gameplay && board.IsTreeOfWisdom() && !(context.flags & TAO_INPUT_MODAL))
                board.OpenTreeStore();
            else if (gameplay && board.IsZenGarden() && !(context.flags & TAO_INPUT_MODAL))
                board.OpenGardenStore();
            // 谁笑到最后：开局/每旗间隙宿主拒绝冻结，Tab 点击宿主导出的
            // "展开攻击"无障碍节点开始进攻；按钮消失后（攻击进行中）回退冻结。
            else if (gameplay && board.TryStartLastStandAttack()) { }
            else if (context.board.object_id && !(context.flags & TAO_INPUT_MODAL)) board.ToggleFreeze();
            else uiNav.Next(Input().Down(tao::KeyCode(16)) ? -1 : 1);
            return true;
        case Action::CycleLeft:
        case Action::CycleRight:
            if (storeFallback) store.NextButton(action == Action::CycleRight ? 1 : -1);
            else if (gameplay && !board.IsTreeOfWisdom()) board.CycleSeed(action == Action::CycleRight ? 1 : -1);
            else if (choosing) chooser.CycleChosen(action == Action::CycleRight ? 1 : -1);
            else uiNav.Next(action == Action::CycleRight ? 1 : -1);
            return true;
        case Action::Info1:
            Say(lastSpoken); // 僵尸声纳待迁移，先提供重读
            return true;
        case Action::Info2:
            if (mainMenu.ReadSummary(context.scene, context.flags)) return true;
            if (gameplay) board.ReadCell();
            return gameplay;
        case Action::Info3:
            if (gameplay) board.AnnounceStatus();
            return gameplay;
        case Action::Info4:
            if (mainMenu.ReadProgress(context.scene, context.flags)) return true;
            if (gameplay) board.AnnounceWave();
            return gameplay;
        case Action::Slot:
            // 数字键选数字卡槽：种子银行在前、场上掉落卡在后。砸花瓶（Vasebreaker）
            // 关卡两者并存（种子银行里可能有植物，场上还可能有砸罐掉落的植物卡）；
            // 老虎机无种子银行（仅掉落卡）。禅境花园数字键改选工具（1..8）。
            if (gameplay) {
                if (board.IsZenGarden()) board.SelectGardenTool(param);
                else if (board.IsTreeOfWisdom()) Say("智慧树没有卡槽，回车施肥");
                else board.SelectCardSlot(size_t(param));
            }
            return gameplay;
        case Action::SlotPull:
            if (gameplay && board.IsSlotMachineLevel()) board.PullSlot();
            return gameplay;
        case Action::FireCannon:
            if (gameplay) board.FireReadyCannon();
            return gameplay;
        case Action::ZombieMinus:
        case Action::ZombiePlus:
            if (gameplay) board.CycleZombie(action == Action::ZombiePlus ? 1 : -1);
            return gameplay;
        case Action::ZombieFilter:
            board.ToggleZombieNames();
            if (gameplay && board.HasBoardFocus()) board.ReadCell();
            return true;
        case Action::RowInfo: {
            // 全部是纯朗读、无副作用：单击立即播报，窗口内二连击改播双击内容
            //（新播报打断第一次的朗读）。shiftPending 只作双击窗口标记，不再延迟。
            const auto now = std::chrono::steady_clock::now();
            const bool doublePress = shiftPending && now - shiftPressedAt < kDoubleClickWindow;
            if (!gameplay) {
                // 选卡界面（scene=2，非铲子教学）：单击 Shift 朗读本关出怪，
                // 双击朗读场地与昼夜；名称跟随僵尸简称开关。
                if (context.scene != int(tao::enums::GameScenes::SCENE_LEVEL_INTRO.Value())) return false;
                shiftPending = !doublePress;
                shiftPressedAt = now;
                if (doublePress) board.AnnounceScene();
                else board.AnnounceLevelZombies();
                return true;
            }
            shiftPending = !doublePress;
            shiftPressedAt = now;
            // 禅境花园：Shift 播报花园总览（各需求数量、臭臭状态）。
            if (board.IsZenGarden()) { board.AnnounceGardenStatus(); return true; }
            // 智慧树：Shift 播报树高与操作提示。
            if (board.IsTreeOfWisdom()) { board.AnnounceTreeStatus(); return true; }
            // 双击播本行小推车，单击播本行僵尸。
            if (doublePress) board.AnnounceRowMowers();
            else board.AnnounceRowZombies();
            return true;
        }
        case Action::Shovel:
            if (gameplay) {
                if (context.cursor == tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value()) board.CycleLayer();
                else board.SelectShovel();
            }
            return gameplay;
        case Action::StepFrame:
            if (gameplay) board.StepOnce();
            return gameplay;
        default:
            return false;
        }
    }
    static TaoPluginStatus TAO_PLUGIN_CALL OnInputEvent(void* opaque, TaoPluginInputEventV1* event) {
        auto& self = *static_cast<PvzA11yMod*>(opaque);
        int device = -1;
        int code = event->code;
        // Reserve standard clipboard shortcuts for independent tools such as
        // pvz.toolkit. Ctrl+C must never enqueue the bare-C cannon action.
        if (event->kind == TAO_INPUT_EVENT_KEY_DOWN && (code == 'C' || code == 'V') &&
            PlatformIsKeyHeld(PLATFORM_VK_CONTROL))
            return TAO_PLUGIN_OK;
        if (event->kind == TAO_INPUT_EVENT_KEY_DOWN) device = 0;
        else if (event->kind == TAO_INPUT_EVENT_GAMEPAD_BUTTON_DOWN) device = 1;
        else return TAO_PLUGIN_OK;
        Action action{};
        int param = 0;
        const bool mapped = self.inputMap.Find(device, code, event->repeat != 0, action, param);
        // 自建日志面板在原生多层模态之上，先于普通/嵌套模态分流；回调只入队。
        if (mapped && self.Enabled() && (action == Action::Settings || self.loggingPanel.Active())) {
            if (action == Action::Settings && !self.loggingPanel.Active()) {
                self.settingsReturnFocus = self.uiNav.Focused();
                self.loggingPanel.RequestOpen();
            } else self.loggingPanel.Queue(action, PlatformIsKeyHeld(PLATFORM_VK_SHIFT) ? -1 : 1);
            event->action_flags |= TAO_INPUT_EVENT_CONSUME;
            return TAO_PLUGIN_OK;
        }
        // 编辑框先于嵌套模态分流：左右、退格、回车、Tab 和组合键交还原生编辑器，
        // 防止在原生模态循环里被解释为导航按钮或关闭弹窗。
        if (device == 0) {
            TaoPluginAccessibilityContextV1 editContext{};
            editContext.struct_size = sizeof(editContext);
            if (self.services.a11y.context(self.services.a11y.host_context, &editContext) == TAO_PLUGIN_OK &&
                (editContext.flags & TAO_INPUT_TEXT_EDIT)) {
                if (mapped && action == Action::Info1 && self.Enabled()) {
                    self.editReadPending = true;
                    event->action_flags |= TAO_INPUT_EVENT_CONSUME;
                    return TAO_PLUGIN_OK;
                }
                if (!(mapped && (action == Action::ToggleMod || action == Action::Help || action == Action::DumpTree)))
                    return TAO_PLUGIN_OK;
            }
        }
        // 阻塞型激活（消息框模态循环）在栈期间，或原生模态循环的嵌套泵帧期间
        //（商店购买走原生 PurchaseItem 直调，弹出的 [BUY_ITEM] 确认框在 exe 内部
        // 跑模态循环，无 invokeguard——2026-10-09 实测按键落 pendingActions 永不
        // 分发、原生对话框又不响应键盘，键盘被吞）：确认/退格转为"模态关闭"请求，
        // 由嵌套帧关闭（确认=激活焦点按钮，退格=CancelDialog，详见 OnUpdate）。
        // 其余按键放行原生。
        if (invokeguard::blocked() || (self.inUpdate && self.openDialogs > 0)) {
            if (mapped && self.Enabled() && self.store.Browsing() &&
                (action == Action::Up || action == Action::Down || action == Action::Left ||
                    action == Action::Right || action == Action::Confirm || action == Action::Deny ||
                    action == Action::Option || action == Action::CycleLeft || action == Action::CycleRight ||
                    action == Action::Store || action == Action::Info1)) {
                if (self.nestedStoreActions.size() < 8) self.nestedStoreActions.push_back({action, param});
                event->action_flags |= TAO_INPUT_EVENT_CONSUME;
                return TAO_PLUGIN_OK;
            }
            if (mapped && (action == Action::Confirm || action == Action::Deny)) {
                if (self.modalDismissPending < 4) ++self.modalDismissPending;
                self.modalDismissAction = action == Action::Deny ? 2 : 1; // 1=确认 2=退格
                event->action_flags |= TAO_INPUT_EVENT_CONSUME;
                self.LogInfo("激活进行中，回车/退格转模态关闭 code=" + std::to_string(code));
            } else if (mapped && (action == Action::Up || action == Action::Down ||
                         action == Action::Left || action == Action::Right || action == Action::Option)) {
                if (self.modalNavQueue.size() < 8)
                    self.modalNavQueue.push_back({action, PlatformIsKeyHeld(PLATFORM_VK_SHIFT) ? -1 : 1});
                event->action_flags |= TAO_INPUT_EVENT_CONSUME;
            } else if (mapped && action == Action::DumpTree) {
                self.nestedDumpPending = true;
                event->action_flags |= TAO_INPUT_EVENT_CONSUME;
            } else if (mapped && self.debugKeys.size() < 64) {
                self.debugKeys.push_back(-int(code));
            }
            return TAO_PLUGIN_OK;
        }
        // 宿主键码表对 OEM 标点统一上报 0（实测逗号/句号/减号/等号/分号/括号均如此，
        // 仅反引号上报 146），也不发 TEXT 事件；标记后由 OnUpdate 用 Win32 物理
        // 键态甄别具体是哪个标点，见 PollOemPunctuation。
        if (device == 0 && code == 0) self.unknownKeyPending = true;
        try {
            if (mapped) {
                if (self.ShouldConsume(action)) {
                    if (self.pendingActions.size() < 32) self.pendingActions.push_back({action, param});
                    event->action_flags |= TAO_INPUT_EVENT_CONSUME;
                } else if (self.debugKeys.size() < 64) {
                    self.debugKeys.push_back(-int(code)); // 临时调试：已映射但未消费，记负数
                }
            } else if (device == 0 && code != 0 && self.debugKeys.size() < 64) {
                // 临时调试：未映射键码（跳过键码 0 的 OEM 标点与 TEXT 事件，避免刷屏）
                self.debugKeys.push_back(code);
            }
        } catch (const std::exception& e) {
            self.LogInfo(std::string("输入处理异常：") + e.what());
        } catch (...) {
            self.LogInfo("输入处理未知异常");
        }
        return TAO_PLUGIN_OK;
    }

    HostServices services;
    Speech speech;
    Settings settings;
    LoggingOptions loggingOptions;
    LoggingPanel loggingPanel;
    TaoPluginAccessibleNodeV2 settingsReturnFocus{};
    uint64_t nextLogConfigPoll = 0;
    Narrator narrator;
    BoardControl board;
    UiNavigation uiNav;
    bool editReadPending = false;
    SeedChooser chooser;
    ImitaterPicker imitater; // 模仿者对话框导航（子树被宿主 BLOCKED，自建虚拟焦点）
    InputMap inputMap;
    Award award;
    ChallengeNames challenges;
    MainMenu mainMenu;
    Store store;
    HostGuards guards;
    TaoPluginToken keys = 0;
    std::string lastSpoken;
    std::string openDialogText;
    std::string pendingScene;
    std::vector<std::string> dialogSpeeches;
    int openDialogs = 0;
    // 弹窗状态整体重置：停订宿主观测期间（老虎机已拉杆/我是僵尸无弹窗段）弹窗
    // 关闭事件会丢失，openDialogs 与弹窗锚点泄漏后 Nodes() 只遍历已销毁的锚点
    // 子树，表现为此后所有界面"当前没有可操作控件"（2026-10-08 挑战界面实测）。
    // 恢复订阅时若无模态弹窗却仍有未关闭计数，即判定泄漏并重置。
    bool RestoreStoreNavigation() {
        store.SetAnchor({});
        if (!store.Browsing()) return false;
        openDialogs = 1;
        openDialogText = CopyView(board.Context().title);
        dialogSpeeches.clear();
        uiNav.SyncFocus({});
        uiNav.SetDialogAnchor(store.Anchor());
        modalDismissPending = 0;
        modalDismissAction = 0;
        modalNavQueue.clear();
        return true;
    }
    void ResetDialogTracking() {
        openDialogs = 0;
        openDialogText.clear();
        dialogSpeeches.clear();
        uiNav.SyncFocus({});
        uiNav.SetDialogAnchor({});
        store.SetAnchor({});
        board.SetBoardFocus(false);
        if (!pendingScene.empty()) {
            if (Enabled()) Say(std::move(pendingScene), false);
            pendingScene.clear();
        }
    }
    // OEM 标点甄别：宿主对这类键只报"有键按下"（键码 0），这里按 VK 读物理
    // 键态确定是哪一个。事件来自游戏自身输入管线，无需再做前台判断。
    void PollOemPunctuation() {
        if (!Enabled()) return;
        static const struct { int vk; Action action; } oemKeys[] = {
            {188, Action::ZombieMinus}, {190, Action::ZombiePlus},
            {189, Action::CycleLeft}, {187, Action::CycleRight},
        };
        for (const auto& key : oemKeys) {
            if (PlatformIsKeyHeld(key.vk)) {
                if (ShouldConsume(key.action) && pendingActions.size() < 32)
                    pendingActions.push_back({key.action, 0});
                return; // 一次只甄别一个键
            }
        }
    }

    struct PendingAction { Action action; int param; };
    std::vector<PendingAction> pendingActions;
    std::vector<PendingAction> nestedStoreActions;
    bool inStoreAction = false;
    bool inUpdate = false; // OnUpdate 可重入闩锁（见 OnUpdate 头注释）
    int nestedTraceCount = 0; // 嵌套帧追踪计数（限流日志用）
    int modalDismissPending = 0; // 阻塞激活/原生模态循环期间累积的"模态关闭"请求数（回车/退格）
    int modalDismissAction = 0;  // 最近一次模态关闭请求的动作：1=回车(激活焦点) 2=退格(CancelDialog)
    std::vector<PendingAction> modalNavQueue; // 阻塞激活期间方向键和 Tab（param=循环方向）
    bool nestedDumpPending = false;
    bool resubscribeNeeded = false; // 模态关闭停订后，外层帧需恢复宿主观测
    std::vector<int> debugKeys; // 临时调试
    bool unknownKeyPending = false;
    bool shiftPending = false;
    bool slotProbe = false; // 老虎机关卡状态采样
    bool izombieProbe = false; // 我是僵尸关卡状态采样
    bool slotPulled = false; // 老虎机关卡是否已拉过杆（决定是否停订宿主观测）
    bool hostSuppressed = false; // 当前是否已停订宿主观测
    bool slotModalSeen = false; // 停订期间补播弹窗去重
    bool slotAdviceShown = false; // 老虎机底部建议栏上一拍的显示状态
    std::string lastSlotResult; // 上次已播报的老虎机结果
    int lastRollCount = -1; // 老虎机拉杆计数记忆
    uint64_t slotSpinHoldUntil = 0; // 拉杆后暂缓自动收集的截止 tick
    static constexpr uint64_t kSlotSpinHold = 500; // 暂缓窗口（~5s）
    std::chrono::steady_clock::time_point shiftPressedAt{};
};

TAO_DEFINE_MOD(PvzA11yMod, "pvz.a11y", "PvZ 无障碍辅助", "0.1.0")

// 死亡方式鉴别：exit() 路径才会跑静态析构（见 AbortWatch.hpp alive.log 说明）。
static abortwatch::ExitProbe g_exitProbe;

#ifdef _WIN32
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) abortwatch::AppendNote("DLL_PROCESS_DETACH");
    return TRUE;
}
#endif
