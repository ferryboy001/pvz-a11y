// SPDX-License-Identifier: LGPL-3.0-or-later
// 选卡界面（scene=2）导航策略：宿主在卡片选中后把控件搬到顶部已选行，
// 原生焦点跟随会打乱键盘导航顺序。本模块按 SeedType 钉住每张卡首次在
// 网格中观察到的位置，方向键始终按钉住位置评分移动；选中/取消后按卡片
// 身份找回节点并驻留原格；加减号在已选卡片间按已选行从左到右循环。
// 焦点节点仍由 UiNavigation 持有，朗读与宿主焦点事件去重沿用原路径。
#pragma once
#include "HostServices.hpp"
#include "TraceLog.hpp"
#include "UiNavigation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

bool A11ySeedChooserAnimating();
bool A11ySeedChooserAvailable();

class SeedChooser {
public:
    explicit SeedChooser(const HostServices* host, std::function<bool()> animationProbe = A11ySeedChooserAnimating,
        std::function<bool()> availabilityProbe = A11ySeedChooserAvailable)
        : host(host), animationProbe(std::move(animationProbe)), availabilityProbe(std::move(availabilityProbe)) {}
    void SetSay(std::function<void(std::string, bool)> sink) { say = std::move(sink); }
    void SetUi(UiNavigation* nav) { uiNav = nav; }

    // 宿主选卡的无障碍子项身份约定：0x200000000+SeedType（模仿者对话框是另一基址）。
    static bool IsCardItem(uint64_t item) { return item >= UINT64_C(0x200000000) && item < UINT64_C(0x200010000); }

    struct Candidate {
        TaoPluginAccessibleNodeV2 node{};
        double px = 0, py = 0; // 方向键评分坐标（卡片为钉住的网格位置）
        double bx = 0;         // 实时中心 x（已选行排序用）
        bool card = false;
        bool chosen = false;
        bool focusable = false; // 宿主上报可聚焦（选满后未选卡被禁用，不可作导航目标）
        bool navigable = false; // 飞行期临时禁用的卡片仍按动画前的可选状态导航
        bool start = false;     // "一起摇滚吧！"按钮（按名称识别）
        std::string label;
    };

    // 原生只读探针判断选择面板在场，避免每次按键先扫一次全树、动作里再扫一次。
    bool Active() const { return availabilityProbe(); }

    // UiNavigation 主动聚焦（含 Tab）意味着用户已选择新的目的地，取消旧驻留。
    void NoteFocus(TaoPluginAccessibleNodeV2 node) {
        logicalFocus = node;
        refocusItem = 0;
        refocusTicks = 0;
        refocusImitaterTicks = 0;
    }
    bool IgnoreHostFocus(TaoPluginAccessibleNodeV2 node) const {
        if (refocusItem && refocusTicks > 0) return node.item != refocusItem;
        return IsCardItem(logicalFocus.item) && !IsCardItem(node.item) && animationProbe();
    }
    void ObserveHostFocus(TaoPluginAccessibleNodeV2 node) {
        NoteFocus(node);
        // 鼠标选卡同样可能触发宿主把焦点临时移到菜单；保留卡片身份。
        if (IsCardItem(node.item) && animationProbe()) {
            refocusItem = node.item;
            refocusTicks = 120;
        }
    }

    void Arrow(int dx, int dy) {
        // 先保留逻辑起点。飞行时所有种子卡 actions=0，宿主焦点可能已退到菜单。
        const auto reference = refocusItem ? TaoPluginAccessibleNodeV2{logicalFocus.widget, refocusItem}
            : (animationProbe() && IsCardItem(logicalFocus.item) ? logicalFocus : uiNav->Focused());
        refocusItem = 0; // 用户主动移动，取消激活后的驻留重试
        refocusImitaterTicks = 0;
        std::vector<Candidate> candidates;
        if (animationProbe() && !cachedCandidates.empty()) {
            candidates = cachedCandidates;
            for (auto& candidate : candidates) if (candidate.card) candidate.focusable = false;
        } else if (!Scan(&candidates)) { Say("当前没有可操作控件"); return; }
        if (candidates.empty()) { Say("当前没有可操作控件"); return; }
        const Candidate* self = nullptr;
        for (const auto& candidate : candidates)
            if (SameNode(candidate.node, reference) ||
                (IsCardItem(reference.item) && candidate.node.item == reference.item)) { self = &candidate; break; }
        Candidate remembered;
        if (!self && IsCardItem(reference.item)) {
            const auto position = pinned.find(uint32_t(reference.item - UINT64_C(0x200000000)));
            if (position == pinned.end()) return; // 旧卡暂不可枚举时，不能退回菜单首项。
            remembered.node = reference;
            remembered.px = position->second.first;
            remembered.py = position->second.second;
            remembered.card = remembered.navigable = true;
            self = &remembered;
        }
        if (!self) {
            for (const auto& candidate : candidates)
                if (candidate.navigable) { MoveFocus(candidate); return; }
            return;
        }
        const Candidate* best = nullptr;
        const bool movingCards = animationProbe() && self->card;
        double bestScore = std::numeric_limits<double>::max();
        for (const auto& candidate : candidates) {
            if (movingCards && !candidate.card) continue; // 全卡临时禁用时，不能把菜单当成卡片邻居。
            if (!candidate.navigable || SameNode(candidate.node, self->node) ||
                (self->card && candidate.node.item == self->node.item)) continue;
            const double primary = dx > 0 ? candidate.px - self->px : dx < 0 ? self->px - candidate.px
                : dy > 0 ? candidate.py - self->py : self->py - candidate.py;
            if (primary <= 1.0) continue;
            const double orthogonal = dx ? std::abs(candidate.py - self->py) : std::abs(candidate.px - self->px);
            // 与 UiNavigation 同一评分：主轴距离 + 3 倍侧向偏移。
            const double score = primary + orthogonal * 3.0;
            if (score < bestScore) { bestScore = score; best = &candidate; }
        }
        // 该方向没有候选（行首/行尾、首行/末行）时留在原位重读当前项。
        MoveFocus(best ? *best : *self);
    }

    void ActivateFocused() {
        if (!animationProbe()) {
            std::vector<Candidate> candidates;
            Scan(&candidates); // 激活前更新稳定网格与可选状态，动画期间不全树扫描。
        }
        const TaoPluginAccessibleNodeV2 focus = uiNav->Focused();
        if (!focus.widget.object_id) { Arrow(1, 0); return; }
        if (!IsCardItem(focus.item)) { uiNav->ActivateFocused(); return; }
        invokeguard::Guard invokeGuard; // 阻塞型激活在栈标志：期间按键放行原生（见 HostServices.hpp）
        const auto status = host->ui.invoke(host->ui.host_context, focus, TAO_ACCESSIBLE_ACTION_ACTIVATE, 0, {});
        if (status != TAO_PLUGIN_OK) { Say("此控件不能激活"); return; }
        // 卡片选中/取消后控件搬家（网格 <-> 已选行），旧节点可能已失效；
        // 按 SeedType 身份重新查找并驻留，保持键盘焦点排列不变。卡片飞行
        // 动画期间不可聚焦（FOCUS fail），找不到时交给 Pump 逐帧重试。
        refocusItem = focus.item;
        logicalFocus = focus;
        refocusTicks = 120; // 隔帧扫描，120 帧 ≈ 60 次重试 ≈ 1 秒
        Pump();
    }

    // 激活后的驻留重试：隔帧一次（Scan 是全树遍历，每帧扫会叠爆 OnUpdate 预算，
    // 宿主对超时会永久禁用更新回调），找到即聚焦并停止（由 Mod 的 OnUpdate 驱动）。
    // 驻留完成（或超时放弃）后若卡槽已选满，把焦点移交"一起摇滚吧！"，
    // 此时未选卡均被宿主禁用，继续留在卡片上方向键会落空。
    void Pump() {
        TaoPluginAccessibilityContextV1 context{};
        context.struct_size = sizeof(context);
        if (host->a11y.context(host->a11y.host_context, &context) != TAO_PLUGIN_OK) return;
        if (context.scene != 2 || !Active()) {
            NoteFocus({});
            eligible.clear();
            cachedCandidates.clear();
            primed = false;
            return;
        }
        if (context.flags & (uint32_t(TAO_INPUT_MODAL) | TAO_INPUT_TEXT_EDIT)) return;
        if (!primed && !animationProbe()) {
            primed = true;
            std::vector<Candidate> candidates;
            Scan(&candidates); // 首次鼠标选卡前也准备稳定网格，只在本次进入时预采样一次。
        }
        // 模仿者对话框关闭后的焦点归还：选定模仿对象时卡片要等对话框关闭后
        // 才进已选行（2026-10-10 实测 DIALOG_CLOSE 当帧 Scan 找不到），单次
        // 扫描落空即永久丢失焦点，与卡片驻留同节奏隔帧重试至出现或超时。
        if (refocusImitaterTicks > 0 && !animationProbe()) {
            --refocusImitaterTicks;
            if (!(refocusImitaterTicks & 1)) TryRefocusImitater();
            if (!refocusImitaterTicks) tracefile::Write("chooser imitater refocus timeout");
        }
        if (!refocusItem || refocusTicks <= 0) return;
        if (animationProbe()) return; // 宿主已知不能 FOCUS，等待结束即可，避免每两帧扫完整卡表。
        --refocusTicks;
        if (refocusTicks & 1) return; // 隔帧扫描，减半 OnUpdate 峰值
        std::vector<Candidate> candidates;
        if (Scan(&candidates))
            for (const auto& candidate : candidates)
                if (candidate.node.item == refocusItem && candidate.focusable && uiNav->Focus(candidate.node)) {
                    refocusItem = 0;
                    FocusStartIfFull(candidates);
                    return;
                }
        // 最后一张卡飞行结束后，目标未选卡会真正禁用，仍应转交开始按钮。
        if (!animationProbe()) FocusStartIfFull(candidates);
        if (!refocusItem) return;
        if (!refocusTicks) { // 超时放弃：焦点留在原节点，等宿主焦点事件校正
            refocusItem = 0;
            FocusStartIfFull(candidates);
        }
    }

    // 加减号：在已选卡片间循环（按已选行实时位置从左到右）。
    void CycleChosen(int direction) {
        refocusItem = 0;
        refocusImitaterTicks = 0;
        std::vector<Candidate> candidates;
        Scan(&candidates);
        std::vector<Candidate> chosen;
        for (const auto& candidate : candidates)
            if (candidate.card && candidate.chosen) chosen.push_back(candidate);
        if (chosen.empty()) { Say("还没有选中植物"); return; }
        std::sort(chosen.begin(), chosen.end(), [](const Candidate& a, const Candidate& b) { return a.bx < b.bx; });
        const TaoPluginAccessibleNodeV2 focus = uiNav->Focused();
        size_t index = direction > 0 ? 0 : chosen.size() - 1;
        for (size_t i = 0; i < chosen.size(); ++i)
            if (SameNode(chosen[i].node, focus)) {
                index = (i + direction + chosen.size()) % chosen.size();
                break;
            }
        uiNav->Focus(chosen[index].node);
    }

private:
    void MoveFocus(const Candidate& candidate) {
        if (candidate.focusable && uiNav->Focus(candidate.node)) {
            NoteFocus(candidate.node);
            return;
        }
        if (!candidate.card || !candidate.navigable) return;
        logicalFocus = candidate.node;
        refocusItem = candidate.node.item;
        refocusTicks = 120;
        uiNav->SyncFocus(candidate.node);
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        if (host->ui.describe(host->ui.host_context, candidate.node, &info) == TAO_PLUGIN_OK) {
            const auto name = uiNav->SpokenName(candidate.node, info);
            if (!name.empty()) Say(name);
        } else if (!candidate.label.empty()) Say(candidate.label);
    }
    bool Scan(std::vector<Candidate>* out) {
        if (out) out->clear();
        std::vector<TaoPluginAccessibleNodeV2> pending;
        size_t count = 0;
        if (host->widgets.root_count(host->widgets.host_context, &count) != TAO_PLUGIN_OK) return false;
        for (size_t i = 0; i < count && i < 1024; ++i) {
            TaoPluginAccessibleNodeV2 node{};
            if (host->widgets.root_at(host->widgets.host_context, i, &node.widget) == TAO_PLUGIN_OK) pending.push_back(node);
        }
        bool found = false;
        const bool animating = animationProbe();
        for (size_t i = 0; i < pending.size(); ++i) {
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, pending[i], &info) != TAO_PLUGIN_OK) continue;
            for (size_t j = 0; j < info.child_count && pending.size() < 1024; ++j) {
                TaoPluginAccessibleNodeV2 child{};
                if (host->ui.child_at(host->ui.host_context, pending[i], j, &child) == TAO_PLUGIN_OK) pending.push_back(child);
            }
            if (info.bounds.width <= 0 || info.bounds.height <= 0) continue;
            const double cx = info.bounds.x + info.bounds.width / 2.0;
            const double cy = info.bounds.y + info.bounds.height / 2.0;
            if (IsCardItem(pending[i].item)) {
                found = true;
                const uint32_t seedType = uint32_t(pending[i].item - UINT64_C(0x200000000));
                // 已选 = SELECTED 标记或已搬到顶部已选行；网格中的位置权威，随时刷新钉住。
                // 只钉落在标准网格晶格上的位置：选中/取消的飞行动画帧坐标处于
                // 网格与已选行之间，SELECTED 标记此时不可靠，直接钉会污染导航。
                const bool chosen = (info.flags & TAO_ACCESSIBLE_SELECTED) != 0 || cy < 100.0;
                if (!chosen && OnGridLattice(cx, cy)) pinned[seedType] = {cx, cy};
                const auto it = pinned.find(seedType);
                Candidate candidate;
                candidate.node = pending[i];
                candidate.px = it != pinned.end() ? it->second.first : cx;
                candidate.py = it != pinned.end() ? it->second.second : cy;
                candidate.bx = cx;
                candidate.card = true;
                candidate.chosen = chosen;
                candidate.label = CopyView(info.name);
                candidate.focusable = (info.actions & TAO_ACCESSIBLE_ACTION_FOCUS) != 0 &&
                    (info.flags & TAO_WIDGET_VISIBLE) &&
                    !(info.flags & (uint32_t(TAO_WIDGET_DISABLED) | TAO_ACCESSIBLE_BLOCKED));
                if (!animating) eligible[seedType] = candidate.focusable;
                const auto allowed = eligible.find(seedType);
                candidate.navigable = candidate.focusable || (animating && allowed != eligible.end() && allowed->second &&
                    (info.flags & TAO_WIDGET_VISIBLE) && !(info.flags & TAO_ACCESSIBLE_BLOCKED));
                if (out) out->push_back(candidate);
                continue;
            }
            if (!out) continue;
            if (!(info.actions & (uint32_t(TAO_ACCESSIBLE_ACTION_ACTIVATE) | TAO_ACCESSIBLE_ACTION_SET_VALUE |
                TAO_ACCESSIBLE_ACTION_SET_TEXT))) continue;
            if (!(info.actions & TAO_ACCESSIBLE_ACTION_FOCUS)) continue;
            Candidate candidate;
            candidate.node = pending[i];
            candidate.px = cx;
            candidate.py = cy;
            candidate.bx = cx;
            candidate.focusable = true;
            candidate.navigable = true;
            candidate.start = CopyView(info.name).find("摇滚") != std::string::npos;
            candidate.label = CopyView(info.name);
            out->push_back(candidate);
        }
        if (found && out && !animating) { cachedCandidates = *out; primed = true; }
        return found;
    }
    void Say(std::string text, bool interrupt = true) const { if (say) say(std::move(text), interrupt); }

    // 卡槽选满后把焦点移交"一起摇滚吧！"。三个条件全部来自宿主无障碍树：
    // 开始按钮可用（禁用/隐藏的控件根本不进候选）、已有选卡、且不存在仍可
    // 聚焦的未选卡（宿主按 mSeedsInBank>=mNumPackets 禁用全部未选卡）。
    void FocusStartIfFull(const std::vector<Candidate>& candidates) {
        const Candidate* start = nullptr;
        bool anyChosen = false, canPickMore = false;
        for (const auto& candidate : candidates) {
            if (candidate.start) { if (!start) start = &candidate; continue; }
            if (!candidate.card) continue;
            if (candidate.chosen) anyChosen = true;
            else if (candidate.focusable) canPickMore = true;
        }
        if (!start || !anyChosen || canPickMore) return;
        if (uiNav->Focus(start->node, false)) Say("选卡完毕，一起摇滚吧！");
    }

public:
    // 模仿者对话框关闭后把焦点还给模仿者按钮（ImitaterPicker 经 DIALOG_CLOSE
    // 触发）。模仿者按钮是非卡候选中唯一的 50x70 按钮（宿主按 GameButton 暴露、
    // mLabel 为空，2026-10-10 DumpTree 实证 bounds=(464,515,50,70)），经 Pump
    // 逐帧驻留直到聚焦成功。
    void RefocusImitaterButton() {
        refocusImitaterTicks = 120; // 卡片迟一帧出现：由 Pump 隔帧重试（≈1 秒）
        if (!animationProbe()) TryRefocusImitater();
    }

private:
    void TryRefocusImitater() {
        std::vector<Candidate> candidates;
        if (!Scan(&candidates)) return;
        // 选定模仿对象后按钮被宿主禁用（flags=[VDFU]，2026-10-10 实测），不可再
        // 聚焦；此时焦点应落到已选行里的模仿者卡（朗读"模仿者，已选入卡槽"形成闭环）。
        for (const auto& candidate : candidates)
            if (candidate.card && candidate.focusable &&
                candidate.node.item == UINT64_C(0x200000000) + uint64_t(tao::enums::SeedType::SEED_IMITATER.Value())) {
                refocusImitaterTicks = 0;
                logicalFocus = candidate.node;
                refocusItem = candidate.node.item;
                refocusTicks = 120;
                tracefile::Write("chooser imitater refocus -> bank card");
                return;
            }
        // 取消（未选定模仿对象）时按钮仍可用，焦点归还按钮。
        for (const auto& candidate : candidates) {
            if (candidate.card || !candidate.focusable) continue;
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, candidate.node, &info) != TAO_PLUGIN_OK) continue;
            if (int(info.bounds.width) == 50 && int(info.bounds.height) == 70) {
                refocusImitaterTicks = 0;
                logicalFocus = candidate.node;
                refocusItem = candidate.node.item;
                refocusTicks = 120;
                tracefile::Write("chooser imitater refocus -> button");
                return;
            }
        }
    }

public:
    // 选卡网格晶格：列 x=22+53k、行 y=123+70k（卡片左上角，容差 3px）。
    // 本版为 6 行布局（第六行是升级卡：双子向日葵等），2026-10-10 DumpTree
    // 实测行坐标 123/193/263/333/403/473；旧常量 y=128+73k（原版 5 行布局）
    // 与全部行偏差 >3px，曾导致钉住永不生效、方向键评分退化到实时坐标。
    static bool OnGridLattice(double cx, double cy) {
        const double x = cx - 25.0, y = cy - 35.0; // 中心 → 左上角
        const double lx = std::fmod(x - 22.0, 53.0);
        const double ly = std::fmod(y - 123.0, 70.0);
        const bool colOk = std::abs(lx) <= 3.0 || std::abs(lx - 53.0) <= 3.0 || std::abs(lx + 53.0) <= 3.0;
        const bool rowOk = std::abs(ly) <= 3.0 || std::abs(ly - 70.0) <= 3.0 || std::abs(ly + 70.0) <= 3.0;
        return colOk && rowOk && x >= 0.0 && y >= 100.0;
    }

    const HostServices* host;
    std::function<bool()> animationProbe;
    std::function<bool()> availabilityProbe;
    UiNavigation* uiNav = nullptr;
    std::function<void(std::string, bool)> say;
    // SeedType -> 钉住的网格中心坐标；选卡网格布局按 SeedType 固定，可跨关卡保留。
    std::unordered_map<uint32_t, std::pair<double, double>> pinned;
    std::unordered_map<uint32_t, bool> eligible;
    std::vector<Candidate> cachedCandidates;
    TaoPluginAccessibleNodeV2 logicalFocus{};
    bool primed = false;
    uint64_t refocusItem = 0; // 激活后待驻留的卡片身份（Pump 逐帧重试）
    int refocusTicks = 0;
    int refocusImitaterTicks = 0; // 模仿者对话框关闭后的焦点归还重试（TryRefocusImitater）
};
