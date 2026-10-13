// SPDX-License-Identifier: LGPL-3.0-or-later
// 控件树遍历与焦点导航：把原生控件和自绘语义项统一为可朗读、可操作节点。
// 对应 PvZA11y 的 Widgets/ 目录；后续设置面板、重绑定界面也经此模块朗读。
#pragma once
#include "HostServices.hpp"
#include "EditFeedback.hpp"
#include "TraceLog.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

class UiNavigation {
public:
    explicit UiNavigation(const HostServices* host) : host(host) {}

    // 主菜单等界面的按钮在语义层没有名称（mAccessibleName 为空），
    // 按逻辑坐标对照表补全显示文本。widget id 随创建顺序漂移（同一按钮
    // 不同会话可能是 6 也可能是 43），坐标才是稳定键；面板滑动会让 x
    // 偏移 ±800，比较前先归一化到 [0,800)。
    static std::string LookupWidgetName(const TaoPluginAccessibleInfoV2& info) {
        if (info.role != TAO_ACCESSIBLE_ROLE_BUTTON) return {};
        struct Entry { int x, y; const char* name; };
        static const Entry entries[] = {
            {405, 79, "冒险模式"},
            {406, 173, "迷你游戏"},
            {410, 257, "解谜模式"},
            {413, 328, "生存模式"},
            {327, 428, "图鉴"}, // 订阅宿主观测期间打开图鉴必崩（宿主空棋盘解引用，见 SDK-feedback.md 缺陷 D），ActivateFocused 拒绝激活；鼠标点击由 HostGuards 拦截 DoAlmanacDialog 兜底
            {171, 401, "禅境花园"}, // 实机确认（2026-10-09 点击该按钮进入花园）；锁定时按钮不在树上
            {405, 482, "商店"},
            {565, 475, "选项"},
            {647, 499, "帮助"},
            {715, 510, "退出"},
            {20, 400, "成就"},
            {32, 180, "僵尸大头贴"},
            {49, 136, "更改用户"},
            {265, 364, "联机模式"},
            {278, 528, "返回"},
            {65, 472, "更改用户名"},
            {445, 472, "删除用户"},
        };
        int x = int(info.bounds.x) % 800;
        if (x < 0) x += 800;
        const int y = int(info.bounds.y);
        for (const auto& entry : entries)
            if (std::abs(x - entry.x) <= 8 && std::abs(y - entry.y) <= 8) return entry.name;
        return {};
    }

    void SetSay(std::function<void(std::string, bool)> sink) { say = std::move(sink); }
    void SetOnFocus(std::function<void()> callback) { onFocus = std::move(callback); }
    // 特殊子项（选卡卡片等）的朗读素材：入参为无障碍子项身份，出参为名称前的
    // 限制前缀与名称后的附加文本；返回 false 表示普通控件，按默认路径朗读。
    void SetSeedInfo(std::function<bool(uint64_t, std::string&, std::string&)> sink) { seedInfo = std::move(sink); }
    // 挑战选择界面（scene=7）关卡按钮的命名查询：宿主不暴露按钮文本，由
    // ChallengeNames 按场景、角色与控件 bounds 反查关卡名；空串表示未命中。
    void SetChallengeName(std::function<std::string(int, uint32_t, TaoPluginRect)> sink) { challengeName = std::move(sink); }
    // 未命名控件的补充命名（商店翻页/返回按钮、主菜单商店入口）；空串表示不处理。
    void SetButtonName(std::function<std::string(const TaoPluginAccessibleInfoV2&)> sink) { buttonName = std::move(sink); }
    void SetMainMenuName(std::function<std::string(const TaoPluginAccessibleInfoV2&)> sink) { mainMenuName = std::move(sink); }
    void SetSpeechDetails(std::function<std::string(const std::string&, const TaoPluginAccessibleInfoV2&)> sink) {
        speechDetails = std::move(sink);
    }
    // 语义子项（商店商品等）名称后的补充文本（价格/是否买得起）；空串表示不追加。
    void SetItemSuffix(std::function<std::string(const TaoPluginAccessibleNodeV2&, const TaoPluginAccessibleInfoV2&)> sink) {
        itemSuffix = std::move(sink);
    }
    // 激活前的通知：供上层记录候选、播报翻页方向等；返回 true 表示上层已用
    // 别的方式处理（如商店入口改走导航请求），跳过本层的语义激活。
    void SetBeforeActivate(std::function<bool(const TaoPluginAccessibleNodeV2&, const TaoPluginAccessibleInfoV2&, const std::string&)> sink) {
        beforeActivate = std::move(sink);
    }

    // 宿主对 Board 控件（存档恢复的棋盘）describe 时会调用 BoardActionGate 崩溃
    //（0xC0000005，宿主缺陷，mod 无法修补）：遍历控件树时按 widgets.describe 的
    // type_name 识别并整棵跳过。仅老虎机模式由上层置位。
    void SetSkipBoardWidget(bool on) { skipBoard = on; }
    // 弹窗锚点：弹窗（如商店 StoreScreen）不在 widgets.root_* 里，只按根控件遍历会
    // 漏掉它，只看到底下的主菜单。打开弹窗时把事件节点设为锚点，遍历改从弹窗子树开始；
    // 关闭时清空。传入 item 节点时归一化到其所属控件。
    void SetDialogAnchor(TaoPluginAccessibleNodeV2 node) {
        node.item = 0;
        dialogAnchor = node;
    }
    bool HasFocus() const { return focus.widget.object_id != 0; }
    TaoPluginAccessibleNodeV2 Focused() const { return focus; }
    void SyncFocus(TaoPluginAccessibleNodeV2 node) {
        focus = node;
        if (!node.widget.object_id) ResetEdit();
    }
    void ResetEdit() { editFeedback.Reset(); editTitleNode = {}; }

    // 单节点采样，不扫描整棵树；也适用于鼠标、原生 Tab 导航和没有订阅事件的帧。
    void PumpEdit() {
        TaoPluginAccessibilityContextV1 context{};
        context.struct_size = sizeof(context);
        if (host->a11y.context(host->a11y.host_context, &context) != TAO_PLUGIN_OK ||
            !(context.flags & TAO_INPUT_TEXT_EDIT) || !context.focus.widget.object_id) {
            ResetEdit(); return;
        }
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        if (host->ui.describe(host->ui.host_context, context.focus, &info) != TAO_PLUGIN_OK ||
            info.role != TAO_ACCESSIBLE_ROLE_EDIT || !(info.flags & TAO_WIDGET_FOCUSED) ||
            (info.flags & (uint32_t(TAO_WIDGET_DISABLED) | TAO_ACCESSIBLE_BLOCKED))) {
            ResetEdit(); return;
        }
        ObserveEdit(context.focus, info);
    }
    bool ReadCurrentEdit() {
        TaoPluginAccessibilityContextV1 context{};
        context.struct_size = sizeof(context);
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        if (host->a11y.context(host->a11y.host_context, &context) != TAO_PLUGIN_OK ||
            !context.focus.widget.object_id ||
            host->ui.describe(host->ui.host_context, context.focus, &info) != TAO_PLUGIN_OK ||
            info.role != TAO_ACCESSIBLE_ROLE_EDIT || !(info.flags & TAO_WIDGET_FOCUSED)) return false;
        Say(Name(context.focus, info));
        return true;
    }
    void ObserveEdit(TaoPluginAccessibleNodeV2 node, const TaoPluginAccessibleInfoV2& info,
        bool announceEntry = true) {
        // describe 的所有字符串都是借用视图，查标题（会再 describe）之前先复制。
        const std::string text = CopyView(info.text);
        const double value = info.value;
        if (!SameNode(editTitleNode, node)) {
            editTitle = EditTitle(node, info);
            editTitleNode = node;
        }
        focus = node;
        if (onFocus) onFocus();
        const std::string speech = editFeedback.Update(node, text, value, editTitle, announceEntry);
        if (!speech.empty()) Say(speech);
    }

    std::string EditTitle(const TaoPluginAccessibleNodeV2& node, const TaoPluginAccessibleInfoV2& info) const {
        const std::string name = CopyView(info.name), text = CopyView(info.text);
        const TaoPluginRect bounds = info.bounds;
        TaoPluginWidgetInfoV1 widget{};
        widget.struct_size = sizeof(widget);
        if (host->widgets.describe(host->widgets.host_context, node.widget, &widget) != TAO_PLUGIN_OK)
            return name == text ? std::string{} : name;
        // 原生 UI 有显式 AccessibleName；mod UI 的 Edit 构造参数只是当前值，
        // 标题常由旁边 Text 控件提供（如工具箱的阳光、金币和阵型代码）。
        if (!name.empty() && name != text && CopyView(widget.type_name).find("ModUi") == std::string::npos)
            return name;
        if (!widget.parent.object_id) return name == text ? std::string{} : name;
        TaoPluginAccessibleNodeV2 parent{widget.parent, 0};
        TaoPluginAccessibleInfoV2 parentInfo{};
        parentInfo.struct_size = sizeof(parentInfo);
        if (host->ui.describe(host->ui.host_context, parent, &parentInfo) != TAO_PLUGIN_OK)
            return name == text ? std::string{} : name;
        const size_t count = std::min<size_t>(parentInfo.child_count, 64);
        double best = std::numeric_limits<double>::max();
        std::string title;
        for (size_t i = 0; i < count; ++i) {
            TaoPluginAccessibleNodeV2 sibling{};
            if (host->ui.child_at(host->ui.host_context, parent, i, &sibling) != TAO_PLUGIN_OK || SameNode(sibling, node)) continue;
            TaoPluginAccessibleInfoV2 label{};
            label.struct_size = sizeof(label);
            if (host->ui.describe(host->ui.host_context, sibling, &label) != TAO_PLUGIN_OK ||
                label.role != TAO_ACCESSIBLE_ROLE_GENERIC || !(label.flags & TAO_WIDGET_VISIBLE) ||
                (label.actions & TAO_ACCESSIBLE_ACTION_FOCUS)) continue;
            std::string caption = CopyView(label.text);
            if (caption.empty()) caption = CopyView(label.name);
            if (caption.empty()) continue;
            const double leftGap = double(bounds.x - (label.bounds.x + label.bounds.width));
            const double aboveGap = double(bounds.y - (label.bounds.y + label.bounds.height));
            double score = std::numeric_limits<double>::max();
            if (leftGap >= 0 && leftGap <= 160 && std::abs(bounds.y - label.bounds.y) <= 8)
                score = leftGap + std::abs(bounds.y - label.bounds.y) * 3;
            else if (aboveGap >= 0 && aboveGap <= 32 && std::abs(bounds.x - label.bounds.x) <= 8)
                score = 200 + aboveGap;
            if (score < best) { best = score; title = std::move(caption); }
        }
        return title.empty() ? (name == text ? std::string{} : name) : title;
    }

    std::vector<TaoPluginAccessibleNodeV2> Nodes() {
        size_t described = 0; // 子树中 ui.describe 成功的节点数（不含锚点自身）
        std::vector<TaoPluginAccessibleNodeV2> result = CollectNodes(described);
        // 自愈2：锚点子树零可操作节点且没有任何子树节点能通过 ui.describe——停订
        // 宿主观测期间弹窗关闭事件丢失、控件已从无障碍树移除但注册表条目残留的
        // 僵尸态（2026-10-09 老虎机"新的游戏"实测：此后永远"当前没有可操作控件"）。
        // 与"子树还在但按钮暂不可操作"的区别就在 described：后者保留锚点等恢复。
        if (result.empty() && dialogAnchor.widget.object_id && described == 0) {
            Log("弹窗锚点子树已从无障碍树移除（关闭事件丢失），清空锚点恢复根控件遍历");
            dialogAnchor = {};
            result = CollectNodes(described);
        }
        return result;
    }
    // Nodes() 的实际遍历：弹窗打开时只遍历弹窗子树，否则遍历根控件。
    // described 统计子树中 ui.describe 成功的节点数（不含锚点自身），供 Nodes() 识别僵尸锚点。
    std::vector<TaoPluginAccessibleNodeV2> CollectNodes(size_t& described) {
        std::vector<TaoPluginAccessibleNodeV2> pending, result;
        if (dialogAnchor.widget.object_id) {
            // 自愈1：停订宿主观测期间弹窗关闭事件会丢失，锚点可能指向已销毁控件。
            // widgets.describe（控件注册表）与 ui.describe（无障碍树）任一失败都算
            // 死锚点，清空恢复根控件遍历（否则此后永远"没有可操作控件"）。只查注册表
            // 不够：控件销毁后条目可能残留（2026-10-09 老虎机"新的游戏"实测）。
            TaoPluginWidgetInfoV1 w{};
            w.struct_size = sizeof(w);
            TaoPluginAccessibleInfoV2 a{};
            a.struct_size = sizeof(a);
            const bool widgetKnown =
                host->widgets.describe(host->widgets.host_context, dialogAnchor.widget, &w) == TAO_PLUGIN_OK;
            const bool a11yKnown =
                widgetKnown && host->ui.describe(host->ui.host_context, dialogAnchor, &a) == TAO_PLUGIN_OK;
            if (!a11yKnown) {
                Log("弹窗锚点已失效（关闭事件可能丢失），恢复根控件遍历");
                dialogAnchor = {};
            }
        }
        if (dialogAnchor.widget.object_id) {
            pending.push_back(dialogAnchor); // 弹窗打开：只遍历弹窗子树，忽略底层场景
        } else {
            size_t count = 0;
            if (host->widgets.root_count(host->widgets.host_context, &count) != TAO_PLUGIN_OK) return result;
            for (size_t i = 0; i < count && i < 1024; ++i) {
                TaoPluginAccessibleNodeV2 node{};
                if (host->widgets.root_at(host->widgets.host_context, i, &node.widget) == TAO_PLUGIN_OK) pending.push_back(node);
            }
        }
        if (skipBoard) { // 用 widgets.describe 的 type_name 识别 Board 控件并整棵跳过
            std::vector<TaoPluginAccessibleNodeV2> kept;
            kept.reserve(pending.size());
            for (auto& n : pending) {
                TaoPluginWidgetInfoV1 w{};
                w.struct_size = sizeof(w);
                if (host->widgets.describe(host->widgets.host_context, n.widget, &w) == TAO_PLUGIN_OK &&
                    CopyView(w.type_name).find("Board") != std::string::npos) continue; // 跳过棋盘控件
                kept.push_back(n);
            }
            pending.swap(kept);
        }
        for (size_t i = 0; i < pending.size(); ++i) {
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, pending[i], &info) != TAO_PLUGIN_OK) continue;
            const bool isAnchorItself = dialogAnchor.widget.object_id != 0 &&
                pending[i].widget.object_id == dialogAnchor.widget.object_id && pending[i].item == 0;
            if (!isAnchorItself) ++described;
            if (CanFocus(info) && (info.actions & (uint32_t(TAO_ACCESSIBLE_ACTION_ACTIVATE) |
                TAO_ACCESSIBLE_ACTION_SET_VALUE | TAO_ACCESSIBLE_ACTION_SET_TEXT)))
                result.push_back(pending[i]);
            for (size_t j = 0; j < info.child_count && pending.size() < 1024; ++j) {
                TaoPluginAccessibleNodeV2 child{};
                if (host->ui.child_at(host->ui.host_context, pending[i], j, &child) == TAO_PLUGIN_OK) pending.push_back(child);
            }
        }
        return result;
    }
    // 节点显示名合成：语义名 → 坐标对照表 → 挑战关卡反查 → 角色附加 → 种子卡前后缀。
    // Focus 与宿主焦点事件共用，保证两条路径朗读口径一致。
    std::string Name(const TaoPluginAccessibleNodeV2& node, const TaoPluginAccessibleInfoV2& info) const {
        if (info.role == TAO_ACCESSIBLE_ROLE_EDIT) {
            const std::string text = CopyView(info.text);
            const double value = info.value;
            return EditFeedback::Full(EditTitle(node, info), text, value);
        }
        std::string name = CopyView(info.name);
        if (mainMenuName) {
            const std::string actual = mainMenuName(info);
            if (!actual.empty()) name = actual;
        }
        if (name.empty() && info.scene == 1 && !info.dialog.object_id) {
            const auto known = std::find_if(mainMenuCache.begin(), mainMenuCache.end(), [&](const MainMenuEntry& entry) {
                return SameNode(entry.node, node);
            });
            if (known != mainMenuCache.end()) name = mainMenuOrder[known->index];
        }
        if (name.empty()) name = LookupWidgetName(info);
        if (name.empty() && challengeName) name = challengeName(int(info.scene), info.role, info.bounds);
        if (name.empty() && buttonName) name = buttonName(info);
        if (info.role == TAO_ACCESSIBLE_ROLE_SLIDER) {
            name += "，滑块";
            const double range = info.maximum - info.minimum;
            if (range > 0)
                name += "，" + std::to_string(int(std::lround((info.value - info.minimum) / range * 100.0))) + "%";
        } else if (info.role == TAO_ACCESSIBLE_ROLE_CHECKBOX) {
            name += "，复选框，" + std::string(info.value != 0 ? "已选中" : "未选中");
        }
        std::string prefix, suffix;
        if (seedInfo && node.item && seedInfo(node.item, prefix, suffix)) {
            // 语序：限制前缀（不推荐/水生/夜间），名称，已选入卡槽，阳光，tooltip。
            if (!prefix.empty()) name = prefix + "，" + name;
            if (info.flags & TAO_ACCESSIBLE_SELECTED) name += "，已选入卡槽";
            name += suffix;
        }
        if (itemSuffix && info.role == TAO_ACCESSIBLE_ROLE_ITEM) {
            const std::string extra = itemSuffix(node, info);
            if (!extra.empty()) name += extra;
        }
        return name;
    }
    std::string SpokenName(const TaoPluginAccessibleNodeV2& node, const TaoPluginAccessibleInfoV2& info) const {
        std::string name = Name(node, info);
        if (speechDetails) name += speechDetails(name, info);
        return name;
    }
    bool Focus(TaoPluginAccessibleNodeV2 node, bool announce = true, bool interrupt = true) {
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        if (host->ui.describe(host->ui.host_context, node, &info) != TAO_PLUGIN_OK || !CanFocus(info)) {
            Log("FOCUS fail id=" + std::to_string(node.widget.object_id) + " (describe/actions)"); // 临时调试
            return false;
        }
        std::string name = SpokenName(node, info);
        const auto focusStatus = host->ui.invoke(host->ui.host_context, node, TAO_ACCESSIBLE_ACTION_FOCUS, 0, {});
        if (focusStatus != TAO_PLUGIN_OK) {
            Log("FOCUS fail id=" + std::to_string(node.widget.object_id) + " invoke=" + std::to_string(int(focusStatus))); // 临时调试
            return false;
        }
        focus = node;
        if (info.role == TAO_ACCESSIBLE_ROLE_EDIT) {
            // Name 可能查询标签并覆盖宿主借用字符串，重新取编辑值作为差量基线。
            info = {}; info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, node, &info) == TAO_PLUGIN_OK)
                ObserveEdit(node, info, false);
        } else editFeedback.Reset();
        if (onFocus) onFocus();
        Log("FOCUS id=" + std::to_string(node.widget.object_id)); // 临时调试
        if (announce) Say(name.empty() ? "未命名控件" : name, interrupt);
        return true;
    }
    void Next(int direction, bool announce = true, bool interrupt = true) {
        auto nodes = Nodes();
        if (nodes.empty()) { Say("当前没有可操作控件"); return; }
        if (MainMenuNext(nodes, direction, announce, interrupt)) return;
        auto it = std::find_if(nodes.begin(), nodes.end(), [&](auto n) { return SameNode(n, focus); });
        int index = it == nodes.end() ? (direction > 0 ? -1 : 0) : int(it - nodes.begin());
        for (size_t i = 0; i < nodes.size(); ++i) {
            index = (index + direction + int(nodes.size())) % int(nodes.size());
            if (Focus(nodes[index], announce, interrupt)) return;
        }
    }
    // 方向键空间导航：按控件 bounds 在视觉网格中移动（选卡界面卡片、模仿者对话框等）。
    // 焦点无有效坐标或该方向没有候选时退回线性循环（Next）。
    void Directional(int dx, int dy, bool announce = true, bool interrupt = true) {
        auto nodes = Nodes();
        if (nodes.empty()) { Say("当前没有可操作控件"); return; }
        const int direction = (dx + dy >= 0) ? 1 : -1;
        if (MainMenuNext(nodes, direction, announce, interrupt)) return;
        TaoPluginAccessibleInfoV2 focusInfo{};
        focusInfo.struct_size = sizeof(focusInfo);
        const bool focusValid = focus.widget.object_id != 0 &&
            std::any_of(nodes.begin(), nodes.end(), [&](auto n) { return SameNode(n, focus); }) &&
            host->ui.describe(host->ui.host_context, focus, &focusInfo) == TAO_PLUGIN_OK &&
            focusInfo.bounds.width > 0 && focusInfo.bounds.height > 0;
        if (!focusValid) { Next(direction, announce, interrupt); return; }
        const double fx = focusInfo.bounds.x + focusInfo.bounds.width / 2.0;
        const double fy = focusInfo.bounds.y + focusInfo.bounds.height / 2.0;
        TaoPluginAccessibleNodeV2 best{};
        double bestScore = std::numeric_limits<double>::max();
        for (const auto& candidate : nodes) {
            if (SameNode(candidate, focus)) continue;
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, candidate, &info) != TAO_PLUGIN_OK) continue;
            if (info.bounds.width <= 0 || info.bounds.height <= 0) continue;
            const double cx = info.bounds.x + info.bounds.width / 2.0;
            const double cy = info.bounds.y + info.bounds.height / 2.0;
            const double primary = dx > 0 ? cx - fx : dx < 0 ? fx - cx : dy > 0 ? cy - fy : fy - cy;
            if (primary <= 1.0) continue;
            const double orthogonal = dx ? std::abs(cy - fy) : std::abs(cx - fx);
            // 主轴距离 + 3 倍侧向偏移：优先同列/同行的最近项，符合视觉网格直觉。
            const double score = primary + orthogonal * 3.0;
            if (score < bestScore) { bestScore = score; best = candidate; }
        }
        if (best.widget.object_id && Focus(best, announce, interrupt)) return;
        // 该方向没有候选（行首/行尾、首行/末行）时不回退线性循环：留在原位重读当前项。
        Focus(focus, announce, interrupt);
    }
    // 非棋盘场景的方向键入口：滑块保持左右调整数值，其余控件按视觉位置移动。
    void Arrow(int dx, int dy) {
        if (dx && !dy) {
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (focus.widget.object_id &&
                host->ui.describe(host->ui.host_context, focus, &info) == TAO_PLUGIN_OK &&
                info.role == TAO_ACCESSIBLE_ROLE_SLIDER) {
                Adjust(dx);
                return;
            }
        }
        Directional(dx, dy);
    }
    // 激活焦点控件。返回是否执行了有效激活（invoke 成功、重复激活守卫或上层
    // beforeActivate 已处理；stale 节点重导航、宿主拒绝、图鉴守卫都算失败），
    // 供上层在停订（失聪）状态下给出"正在切换"提示。
    bool ActivateFocused() {
        tracefile::Write("ActivateFocused: enter id=" + std::to_string(focus.widget.object_id) +
            " item=" + std::to_string(focus.item) + " anchor=" + std::to_string(dialogAnchor.widget.object_id));
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        // 陈旧焦点守卫：弹窗关闭后焦点控件可能已销毁（关闭事件异步送达，期间焦点
        // 不会清空）。describe 失败说明节点已不在控件树里，再 invoke 会对已销毁
        // 控件二次 ButtonPress，宿主随后拆帧时崩溃。此时改走重新导航。
        const bool described = host->ui.describe(host->ui.host_context, focus, &info) == TAO_PLUGIN_OK;
        tracefile::Write("ActivateFocused: describe=" + std::to_string(described) +
            (described ? " role=" + std::to_string(info.role) + " actions=" + std::to_string(info.actions) +
                " flags=" + std::to_string(info.flags) + " children=" + std::to_string(info.child_count) : ""));
        if (!described) {
            Log("ACTIVATE skip: 焦点节点已失效（弹窗可能已关闭），重新导航");
            tracefile::Write("ActivateFocused: skip stale node, re-navigating");
            SyncFocus({});
            Next(1);
            return false;
        }
        const bool isCheckbox = info.role == TAO_ACCESSIBLE_ROLE_CHECKBOX;
        const std::string name = Name(focus, info);
        // 宿主缺陷规避（SDK-feedback.md 缺陷 D）：图鉴弹窗构造时宿主观测管线以空
        // 棋盘调用 Board::GetGridColumns 崩溃，语义激活路径直接拒绝并提示。
        //（原生鼠标点击由 AlmanacGuard 拦截 LawnApp::DoAlmanacDialog 兜底。）
        if (name == "图鉴") { Say("图鉴暂时无法打开：宿主在订阅无障碍观测期间打开图鉴会崩溃。"); return false; }
        // 重复激活守卫：激活弹窗按钮（确定/返回等）会关闭弹窗并销毁该控件，关闭
        // 事件异步送达，窗口内的第二次激活会对正在拆除的控件二次 ButtonPress，宿主
        // 崩溃（2026-10-09 生存模式未解锁对话框双击回车实测闪退）。同一节点短窗内
        // 的重复激活只重读名称，不重复 invoke。
        if (SameNode(focus, lastActivated) && ElapsedSinceActivation() < 400.0) {
            Log("ACTIVATE skip: 同一节点 400ms 内重复激活");
            tracefile::Write("ActivateFocused: skip repeat within 400ms window");
            Say(name.empty() ? "未命名控件" : name, false);
            return true; // 该节点刚激活过，不再重复提示切换
        }
        if (beforeActivate && beforeActivate(focus, info, name)) {
            tracefile::Write("ActivateFocused: handled by beforeActivate (name=" + name + ")");
            return true;
        }
        invokeguard::Guard invokeGuard; // 阻塞型激活在栈标志：期间按键放行原生（见 HostServices.hpp）
        const auto status = host->ui.invoke(host->ui.host_context, focus, TAO_ACCESSIBLE_ACTION_ACTIVATE, 0, {});
        Log("ACTIVATE status=" + std::to_string(int(status)));
        tracefile::Write("ActivateFocused: invoke status=" + std::to_string(int(status)));
        if (status != TAO_PLUGIN_OK) { Say("此控件不能激活"); return false; }
        lastActivated = focus;
        lastActivatedAt = std::chrono::steady_clock::now();
        if (isCheckbox) {
            TaoPluginAccessibleInfoV2 after{};
            after.struct_size = sizeof(after);
            if (host->ui.describe(host->ui.host_context, focus, &after) == TAO_PLUGIN_OK)
                Say(after.value != 0 ? "已选中" : "未选中", false);
        }
        tracefile::Write("ActivateFocused: done id=" + std::to_string(focus.widget.object_id));
        return true;
    }
    void Adjust(int direction) {
        host->ui.invoke(host->ui.host_context, focus,
            direction > 0 ? TAO_ACCESSIBLE_ACTION_INCREMENT : TAO_ACCESSIBLE_ACTION_DECREMENT, 0, {});
        Focus(focus);
    }
    // 焦点控件当前是否可激活（阻塞型模态里用于判断回车能否直接按下）。
    bool FocusedIsActionable() {
        if (!focus.widget.object_id) return false;
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        return host->ui.describe(host->ui.host_context, focus, &info) == TAO_PLUGIN_OK &&
            (info.actions & TAO_ACCESSIBLE_ACTION_ACTIVATE) != 0;
    }
    void CancelDialog() {
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        if (host->ui.describe(host->ui.host_context, focus, &info) != TAO_PLUGIN_OK) return;
        TaoPluginAccessibleNodeV2 node{info.dialog.object_id ? info.dialog : focus.widget, 0};
        host->ui.invoke(host->ui.host_context, node, TAO_ACCESSIBLE_ACTION_CANCEL, 0, {});
    }
    // 停订（失聪）期间升起的新弹窗收不到 DIALOG_OPEN 事件：锚点停在已拆除的
    // 旧弹窗上，Nodes() 永远为空、标题也不播报（2026-10-09 老虎机"新的游戏"
    // 实测：后续询问对话框卡住 3 分钟，靠未映射空格键漏进原生才推进）。用屏幕
    // 中心命中测试找到当前顶层弹窗（经 info.dialog 归一化到所属对话，再按
    // type_name 验证是 Dialog），与当前锚点不同即换锚、补播标题；焦点已随旧
    // 弹窗拆除而失效时，聚焦新弹窗的首个可操作控件，让方向键/回车立即可用。
    void RediscoverDialog() {
        TaoPluginObjectHandle hit{};
        if (host->widgets.at_point(host->widgets.host_context, 400, 300, &hit) != TAO_PLUGIN_OK || !hit.object_id) return;
        TaoPluginAccessibleNodeV2 node{hit, 0};
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        if (host->ui.describe(host->ui.host_context, node, &info) != TAO_PLUGIN_OK) return;
        TaoPluginAccessibleNodeV2 dialog{info.dialog.object_id ? info.dialog : hit, 0};
        if (!dialog.widget.object_id || SameNode(dialog, dialogAnchor)) return;
        TaoPluginWidgetInfoV1 w{};
        w.struct_size = sizeof(w);
        if (host->widgets.describe(host->widgets.host_context, dialog.widget, &w) != TAO_PLUGIN_OK ||
            CopyView(w.type_name).find("Dialog") == std::string::npos) return;
        TaoPluginAccessibleInfoV2 dialogInfo{};
        dialogInfo.struct_size = sizeof(dialogInfo);
        if (host->ui.describe(host->ui.host_context, dialog, &dialogInfo) != TAO_PLUGIN_OK) return;
        SetDialogAnchor(dialog);
        std::string title = Name(dialog, dialogInfo);
        if (title.empty()) title = CopyView(dialogInfo.description);
        if (title.empty()) title = CopyView(dialogInfo.text);
        Log("停订期间发现新弹窗 id=" + std::to_string(dialog.widget.object_id) + "，换锚并补播");
        Say(title.empty() ? "对话框已打开" : title, true);
        TaoPluginAccessibleInfoV2 focusInfo{};
        focusInfo.struct_size = sizeof(focusInfo);
        const bool focusAlive = focus.widget.object_id != 0 &&
            host->ui.describe(host->ui.host_context, focus, &focusInfo) == TAO_PLUGIN_OK &&
            SameHandle(focusInfo.dialog, dialog.widget) && CanFocus(focusInfo);
        if (!focusAlive) Next(1, true, false); // 队列播报首个按钮，不打断标题
    }
    // 临时调试：把整棵无障碍树（含内部名称与控件类型名）写入日志。
    void DumpTree() {
        std::vector<TaoPluginAccessibleNodeV2> pending;
        size_t count = 0;
        if (host->widgets.root_count(host->widgets.host_context, &count) == TAO_PLUGIN_OK) {
            for (size_t i = 0; i < count && i < 1024; ++i) {
                TaoPluginObjectHandle handle{};
                if (host->widgets.root_at(host->widgets.host_context, i, &handle) != TAO_PLUGIN_OK) continue;
                TaoPluginWidgetInfoV1 w{};
                w.struct_size = sizeof(w);
                const bool ok = host->widgets.describe(host->widgets.host_context, handle, &w) == TAO_PLUGIN_OK;
                Log("root[" + std::to_string(i) + "] id=" + std::to_string(handle.object_id) +
                    " type=[" + CopyView(w.type_name) + "] rect=(" + std::to_string(int(w.absolute_rect.x)) + "," +
                    std::to_string(int(w.absolute_rect.y)) + "," + std::to_string(int(w.absolute_rect.width)) + "," +
                    std::to_string(int(w.absolute_rect.height)) + ")" + (ok ? "" : " describe=fail"));
                TaoPluginAccessibleNodeV2 node{};
                node.widget = handle;
                pending.push_back(node);
            }
        }
        if (dialogAnchor.widget.object_id) {
            Log("dialogAnchor id=" + std::to_string(dialogAnchor.widget.object_id));
            pending.push_back(dialogAnchor);
        }
        TaoPluginAccessibilityContextV1 ctx{};
        ctx.struct_size = sizeof(ctx);
        if (host->a11y.context(host->a11y.host_context, &ctx) == TAO_PLUGIN_OK)
            Log("context scene=" + std::to_string(ctx.scene) + " flags=" + std::to_string(ctx.flags) +
                " modal=" + std::to_string((ctx.flags & TAO_INPUT_MODAL) != 0) +
                " focusWidget=" + std::to_string(ctx.focus.widget.object_id));
        Log("=== DumpTree begin ===");
        for (size_t i = 0; i < pending.size(); ++i) {
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, pending[i], &info) != TAO_PLUGIN_OK) continue;
            TaoPluginWidgetInfoV1 w{};
            w.struct_size = sizeof(w);
            host->widgets.describe(host->widgets.host_context, pending[i].widget, &w);
            std::string flags;
            if (info.flags & TAO_WIDGET_VISIBLE) flags += "V";
            if (info.flags & TAO_WIDGET_MOUSE_VISIBLE) flags += "M";
            if (info.flags & TAO_WIDGET_DISABLED) flags += "D";
            if (info.flags & TAO_WIDGET_FOCUSED) flags += "F";
            if (info.flags & TAO_ACCESSIBLE_SELECTED) flags += "S";
            if (info.flags & TAO_ACCESSIBLE_UNNAMED) flags += "U";
            if (info.flags & TAO_ACCESSIBLE_BLOCKED) flags += "B";
            Log("node[" + std::to_string(i) + "] id=" + std::to_string(pending[i].widget.object_id) +
                " item=" + std::to_string(pending[i].item) +
                " type=[" + CopyView(w.type_name) + "]" +
                " role=" + std::to_string(info.role) + " actions=" + std::to_string(info.actions) +
                " flags=[" + flags + "] children=" + std::to_string(info.child_count) +
                " scene=" + std::to_string(info.scene) +
                " bounds=(" + std::to_string(int(info.bounds.x)) + "," + std::to_string(int(info.bounds.y)) + "," +
                std::to_string(int(info.bounds.width)) + "," + std::to_string(int(info.bounds.height)) + ")" +
                " name=[" + CopyView(info.name) + "] desc=[" + CopyView(info.description) + "] text=[" + CopyView(info.text) + "]" +
                HitProbe(pending[i], info));
            for (size_t j = 0; j < info.child_count && pending.size() < 2048; ++j) {
                TaoPluginAccessibleNodeV2 child{};
                if (host->ui.child_at(host->ui.host_context, pending[i], j, &child) == TAO_PLUGIN_OK) pending.push_back(child);
            }
        }
        Log("=== DumpTree end ===");
    }
    // 诊断：控件自身 bounds 中心处命中哪个控件（验证宿主"是否被遮挡"的判定）。
    std::string HitProbe(const TaoPluginAccessibleNodeV2& node, const TaoPluginAccessibleInfoV2& info) const {
        if (node.item || info.bounds.width <= 0 || info.bounds.height <= 0) return {};
        const int x = int(info.bounds.x + info.bounds.width / 2);
        const int y = int(info.bounds.y + info.bounds.height / 2);
        TaoPluginObjectHandle hit{};
        if (host->widgets.at_point(host->widgets.host_context, x, y, &hit) != TAO_PLUGIN_OK) return " hit=fail";
        TaoPluginWidgetInfoV1 w{};
        w.struct_size = sizeof(w);
        host->widgets.describe(host->widgets.host_context, hit, &w);
        return " hit=" + std::to_string(hit.object_id) + "[" + CopyView(w.type_name) + "]";
    }

private:
    static bool CanFocus(const TaoPluginAccessibleInfoV2& info) {
        return (info.actions & TAO_ACCESSIBLE_ACTION_FOCUS) != 0 &&
            (info.flags & TAO_WIDGET_VISIBLE) != 0 &&
            (info.flags & (uint32_t(TAO_WIDGET_DISABLED) | TAO_ACCESSIBLE_BLOCKED)) == 0;
    }
    bool MainMenuNext(const std::vector<TaoPluginAccessibleNodeV2>& nodes, int direction,
        bool announce, bool interrupt) {
        if (dialogAnchor.widget.object_id) return false;
        TaoPluginAccessibilityContextV1 context{};
        context.struct_size = sizeof(context);
        if (host->a11y.context(host->a11y.host_context, &context) != TAO_PLUGIN_OK ||
            context.scene != 1 || (context.flags & TAO_INPUT_MODAL)) return false;
        std::vector<MainMenuEntry> menu;
        for (const auto& node : nodes) {
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, node, &info) != TAO_PLUGIN_OK ||
                info.scene != 1 || info.dialog.object_id || !CanFocus(info)) continue;
            const std::string name = Name(node, info);
            size_t index = 0;
            while (index < std::size(mainMenuOrder) && name != mainMenuOrder[index]) ++index;
            if (index == std::size(mainMenuOrder)) continue;
            TaoPluginObjectHandle ancestor = node.widget;
            // 当前宿主的 widgets.describe 会把原生类型统一报成 Widget、父句柄为空。
            // 主菜单原生按钮字段匹配同样是权威身份，不能只依赖 GameSelector 类型名。
            bool selector = mainMenuName && !mainMenuName(info).empty();
            for (int depth = 0; ancestor.object_id && depth < 16; ++depth) {
                TaoPluginWidgetInfoV1 widgetInfo{};
                widgetInfo.struct_size = sizeof(widgetInfo);
                if (host->widgets.describe(host->widgets.host_context, ancestor, &widgetInfo) != TAO_PLUGIN_OK) break;
                if (CopyView(widgetInfo.type_name).find("GameSelector") != std::string::npos) {
                    if (!SameHandle(mainMenuRoot, ancestor)) {
                        mainMenuCache.clear();
                        mainMenuRoot = ancestor;
                    }
                    selector = true;
                    break;
                }
                ancestor = widgetInfo.parent;
            }
            if (selector) {
                menu.push_back({node, index});
                if (std::none_of(mainMenuCache.begin(), mainMenuCache.end(), [&](const MainMenuEntry& entry) {
                    return SameNode(entry.node, node);
                })) mainMenuCache.push_back({node, index});
            }
        }
        if (menu.empty()) return false;
        std::sort(menu.begin(), menu.end(), [](const MainMenuEntry& first, const MainMenuEntry& second) {
            if (first.index != second.index) return first.index < second.index;
            if (first.node.widget.object_id != second.node.widget.object_id)
                return first.node.widget.object_id < second.node.widget.object_id;
            return first.node.item < second.node.item;
        });
        const auto current = std::find_if(menu.begin(), menu.end(), [&](const MainMenuEntry& entry) {
            return SameNode(entry.node, focus);
        });
        if (HasFocus() && current == menu.end() &&
            std::any_of(nodes.begin(), nodes.end(), [&](auto node) { return SameNode(node, focus); })) return false;
        int index = current == menu.end() ? (direction > 0 ? -1 : 0) : int(current - menu.begin());
        for (size_t attempt = 0; attempt < menu.size(); ++attempt) {
            index = (index + direction + int(menu.size())) % int(menu.size());
            if (Focus(menu[index].node, announce, interrupt)) return true;
        }
        return true;
    }
    void Say(std::string text, bool interrupt = true) const { if (say) say(std::move(text), interrupt); }
    void Log(const std::string& text) const { if (log) log(text); }
    double ElapsedSinceActivation() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lastActivatedAt).count();
    }

    const HostServices* host;
    inline static constexpr const char* mainMenuOrder[] = {
        "冒险模式", "快速游戏", "迷你游戏", "解谜模式", "生存模式", "禅境花园", "商店", "图鉴",
        "成就", "僵尸大头贴", "更改用户", "联机模式", "选项", "帮助", "退出"
    };
    struct MainMenuEntry {
        TaoPluginAccessibleNodeV2 node;
        size_t index;
    };
    TaoPluginObjectHandle mainMenuRoot{};
    std::vector<MainMenuEntry> mainMenuCache;
    std::function<void(std::string, bool)> say;
    std::function<bool(uint64_t, std::string&, std::string&)> seedInfo;
    std::function<std::string(int, uint32_t, TaoPluginRect)> challengeName;
    std::function<std::string(const TaoPluginAccessibleInfoV2&)> buttonName;
    std::function<std::string(const TaoPluginAccessibleInfoV2&)> mainMenuName;
    std::function<std::string(const std::string&, const TaoPluginAccessibleInfoV2&)> speechDetails;
    std::function<std::string(const TaoPluginAccessibleNodeV2&, const TaoPluginAccessibleInfoV2&)> itemSuffix;
    std::function<bool(const TaoPluginAccessibleNodeV2&, const TaoPluginAccessibleInfoV2&, const std::string&)> beforeActivate;
public:
    void SetLog(std::function<void(const std::string&)> sink) { log = std::move(sink); }
private:
    std::function<void(const std::string&)> log;
    bool skipBoard = false;
    std::function<void()> onFocus;
    TaoPluginAccessibleNodeV2 focus{};
    EditFeedback editFeedback;
    TaoPluginAccessibleNodeV2 editTitleNode{};
    std::string editTitle;
    TaoPluginAccessibleNodeV2 dialogAnchor{};
    TaoPluginAccessibleNodeV2 lastActivated{}; // 重复激活守卫：上次成功激活的节点
    std::chrono::steady_clock::time_point lastActivatedAt{}; // 及其时间（400ms 防双击窗口）
};
