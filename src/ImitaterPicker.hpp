// SPDX-License-Identifier: LGPL-3.0-or-later
// 模仿者对话框（ImitaterDialog）导航策略。宿主把整个对话框子树判为 BLOCKED：
// ModAccessibilityUi.inl 的 AccessibleDescribeV2 末尾 CanInteractWithWidget 失败 →
// actions=0 + TAO_ACCESSIBLE_BLOCKED（2026-10-10 跳跳舞会 DumpTree 实证：43 个子项
// 全部 actions=0），语义 FOCUS/ACTIVATE/CANCEL 全被宿主拒绝；但子项的名称与
// bounds 仍可正常 describe。与 Store 同款"语义枚举 + 原生直调"：方向键按 bounds
// 评分移动虚拟焦点并朗读，回车经 TaoEngineResolveObject 解出对话框原生指针、
// 直调 ImitaterDialog::MouseDown（与鼠标点击同一入口，子项中心换算为对话框局部
// 坐标），退格调 SexyAppBase::KillDialog(Dialog*) 关窗。对话框身份依据：子项身份
// 区间 0xd00000000+SeedType 仅 ImitaterDialog 产生（宿主同文件 ImitaterDialog 段）。
#pragma once
#include "HostServices.hpp"
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

// 宿主导出成员函数（lib/TaoEngineHost.def，TaoEngineHost.lib 延迟加载重定向到游戏
// exe；x64 成员调用 ABI = 首参 this 的自由函数，BoardControl.hpp 同款先例）。
extern "C" void* TaoEngineResolveObject(TaoPluginObjectHandle handle) noexcept;
extern "C" void* TaoEngineGetApp() noexcept;
#if defined(_MSC_VER)
extern "C" void __cdecl ImitaterDialog_MouseDown(void* self, int x, int y, int clickCount) noexcept;
extern "C" bool __cdecl SexyAppBase_KillDialogPtr(void* self, void* dialog) noexcept;
#pragma comment(linker, "/alternatename:ImitaterDialog_MouseDown=?MouseDown@ImitaterDialog@@UEAAXHHH@Z")
#pragma comment(linker, "/alternatename:SexyAppBase_KillDialogPtr=?KillDialog@SexyAppBase@Sexy@@UEAA_NPEAVDialog@2@@Z")
#else
extern "C" void _ZN14ImitaterDialog9MouseDownEiii(void* self, int x, int y, int clickCount) noexcept;
extern "C" bool _ZN4Sexy11SexyAppBase10KillDialogEPNS_6DialogE(void* self, void* dialog) noexcept;
inline void ImitaterDialog_MouseDown(void* self, int x, int y, int clickCount) noexcept {
    _ZN14ImitaterDialog9MouseDownEiii(self, x, y, clickCount);
}
inline bool SexyAppBase_KillDialogPtr(void* self, void* dialog) noexcept {
    return _ZN4Sexy11SexyAppBase10KillDialogEPNS_6DialogE(self, dialog);
}
#endif

class ImitaterPicker {
public:
    explicit ImitaterPicker(const HostServices* host) : host(host) {}
    void SetSay(std::function<void(std::string, bool)> sink) { say = std::move(sink); }
    void SetLog(std::function<void(const std::string&)> sink) { log = std::move(sink); }
    // 与选卡卡片同源的朗读素材（限制前缀/阳光/tooltip 后缀）；0xd00000000 区间
    // 由 BoardControl::SeedCardInfo 直接支持。
    void SetSeedInfo(std::function<bool(uint64_t, std::string&, std::string&)> sink) { seedInfo = std::move(sink); }

    // 宿主约定（ModAccessibilityUi.inl）：0xd00000000+SeedType 为对话框内原版植物，
    // bit63 为 mod 植物（AccessibleContentId）。在场检测只认原版区间——图鉴等其它
    // Dialog 的 mod 内容同样是 bit63，单靠它会误判。
    static bool IsNativePickItem(uint64_t item) { return item >= UINT64_C(0xd00000000) && item < UINT64_C(0xd00010000); }
    static bool IsModPickItem(uint64_t item) { return (item & UINT64_C(0x8000000000000000)) != 0; }

    // 模仿者对话框是否在场：根控件中存在带 0xd00000000 子项的 Dialog。
    bool Active() { return Scan(nullptr, nullptr, nullptr); }

    void Arrow(int dx, int dy) {
        std::vector<Entry> entries;
        if (!Scan(nullptr, &entries, nullptr) || entries.empty()) { Say("模仿者对话框已关闭"); return; }
        const Entry* self = nullptr;
        for (const auto& entry : entries)
            if (entry.node.item == focusItem) { self = &entry; break; }
        if (!self) { FocusEntry(entries.front()); return; }
        const Entry* best = nullptr;
        double bestScore = std::numeric_limits<double>::max();
        for (const auto& entry : entries) {
            if (entry.node.item == self->node.item) continue;
            const double primary = dx > 0 ? entry.cx - self->cx : dx < 0 ? self->cx - entry.cx
                : dy > 0 ? entry.cy - self->cy : self->cy - entry.cy;
            if (primary <= 1.0) continue;
            const double orthogonal = dx ? std::abs(entry.cy - self->cy) : std::abs(entry.cx - self->cx);
            // 与 UiNavigation 同一评分：主轴距离 + 3 倍侧向偏移。
            const double score = primary + orthogonal * 3.0;
            if (score < bestScore) { bestScore = score; best = &entry; }
        }
        // 该方向没有候选时留在原位重读当前项。
        FocusEntry(best ? *best : *self);
    }

    void Activate() {
        TaoPluginAccessibleNodeV2 dialog{};
        std::vector<Entry> entries;
        TaoPluginRect bounds{};
        if (!Scan(&dialog, &entries, &bounds)) { Say("模仿者对话框已关闭"); return; }
        const Entry* self = nullptr;
        for (const auto& entry : entries)
            if (entry.node.item == focusItem) { self = &entry; break; }
        if (!self) {
            if (!entries.empty()) FocusEntry(entries.front());
            return;
        }
        // 尊重宿主 enabled 判定（SeedNotAllowedToPick/卡槽已满），不用原生点击
        // 绕过游戏规则；禁用项只重读不激活。
        if (!self->enabled) { Announce(*self, true); return; }
        void* object = TaoEngineResolveObject(dialog.widget);
        if (!object) {
            Log("模仿者选择失败：对话框原生对象解析为空");
            Say("激活失败");
            return;
        }
        const int localX = int(self->cx - bounds.x);
        const int localY = int(self->cy - bounds.y);
        Log("模仿者选择：" + self->name + "（对话框局部坐标 " + std::to_string(localX) +
            "," + std::to_string(localY) + "）");
        // 与鼠标同一入口：ImitaterDialog::MouseDown 内部 SeedHitTest 命中后完成
        // 选卡、刷新模仿者按钮并 KillDialog，对话框关闭事件随后到达。
        ImitaterDialog_MouseDown(object, localX, localY, 1);
    }

    void Cancel() {
        TaoPluginAccessibleNodeV2 dialog{};
        if (!Scan(&dialog, nullptr, nullptr)) { Say("模仿者对话框已关闭"); return; }
        void* object = TaoEngineResolveObject(dialog.widget);
        void* app = TaoEngineGetApp();
        if (!object || !app) {
            Log("模仿者对话框取消失败：原生对象解析为空");
            Say("取消失败");
            return;
        }
        Say("已取消", false);
        SexyAppBase_KillDialogPtr(app, object);
    }

    // DIALOG_OPEN 后调用：标题正文已由上层播报，这里把虚拟焦点放到首个可选项
    // 并排队朗读（不打断正文）。
    void AnnounceEntry() {
        std::vector<Entry> entries;
        if (!Scan(nullptr, &entries, nullptr) || entries.empty()) return;
        focusItem = 0;
        for (const auto& entry : entries)
            if (entry.enabled) { FocusEntry(entry, false); return; }
        FocusEntry(entries.front(), false);
    }

    // 对话框是否曾被观测打开；Plugin 在 DIALOG_CLOSE 时取走该标记决定焦点归还。
    bool ConsumeOpenFlag() {
        const bool seen = openSeen;
        openSeen = false;
        return seen;
    }

private:
    struct Entry {
        TaoPluginAccessibleNodeV2 node{};
        double cx = 0, cy = 0; // bounds 中心（绝对坐标）
        std::string name;
        bool enabled = true;
    };

    // 扫描根控件找模仿者对话框并枚举其子项。返回 false 时清虚拟焦点。
    bool Scan(TaoPluginAccessibleNodeV2* dialogOut, std::vector<Entry>* entriesOut, TaoPluginRect* boundsOut) {
        size_t count = 0;
        if (host->widgets.root_count(host->widgets.host_context, &count) != TAO_PLUGIN_OK) {
            focusItem = 0;
            return false;
        }
        for (size_t i = 0; i < count && i < 64; ++i) {
            TaoPluginObjectHandle handle{};
            if (host->widgets.root_at(host->widgets.host_context, i, &handle) != TAO_PLUGIN_OK) continue;
            TaoPluginWidgetInfoV1 w{};
            w.struct_size = sizeof(w);
            if (host->widgets.describe(host->widgets.host_context, handle, &w) != TAO_PLUGIN_OK) continue;
            if (CopyView(w.type_name).find("Dialog") == std::string::npos) continue;
            TaoPluginAccessibleNodeV2 dialog{handle, 0};
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, dialog, &info) != TAO_PLUGIN_OK) continue;
            std::vector<Entry> entries;
            bool native = false;
            for (size_t j = 0; j < info.child_count; ++j) {
                TaoPluginAccessibleNodeV2 child{};
                if (host->ui.child_at(host->ui.host_context, dialog, j, &child) != TAO_PLUGIN_OK || !child.item) continue;
                const bool isNative = IsNativePickItem(child.item);
                if (!isNative && !IsModPickItem(child.item)) continue;
                TaoPluginAccessibleInfoV2 childInfo{};
                childInfo.struct_size = sizeof(childInfo);
                if (host->ui.describe(host->ui.host_context, child, &childInfo) != TAO_PLUGIN_OK) continue;
                if (childInfo.bounds.width <= 0 || childInfo.bounds.height <= 0) continue;
                native = native || isNative;
                Entry entry;
                entry.node = child;
                entry.cx = childInfo.bounds.x + childInfo.bounds.width / 2.0;
                entry.cy = childInfo.bounds.y + childInfo.bounds.height / 2.0;
                entry.name = CopyView(childInfo.name);
                entry.enabled = (childInfo.flags & TAO_WIDGET_DISABLED) == 0;
                entries.push_back(std::move(entry));
            }
            if (!native || entries.empty()) continue;
            openSeen = true;
            if (dialogOut) *dialogOut = dialog;
            if (boundsOut) *boundsOut = info.bounds;
            if (entriesOut) *entriesOut = std::move(entries);
            return true;
        }
        focusItem = 0;
        return false;
    }

    void FocusEntry(const Entry& entry, bool interrupt = true) {
        focusItem = entry.node.item;
        Announce(entry, interrupt);
    }
    void Announce(const Entry& entry, bool interrupt) {
        std::string text = entry.name.empty() ? "未命名卡片" : entry.name;
        std::string prefix, suffix;
        if (seedInfo && seedInfo(entry.node.item, prefix, suffix)) {
            // 与选卡界面同一语序：限制前缀（不推荐/水生/夜间），名称，阳光+tooltip。
            if (!prefix.empty()) text = prefix + "，" + text;
            text += suffix;
        }
        if (!entry.enabled) text += "，本关不可选";
        Say(std::move(text), interrupt);
    }
    void Say(std::string text, bool interrupt = true) const { if (say) say(std::move(text), interrupt); }
    void Log(const std::string& text) const { if (log) log(text); }

    const HostServices* host;
    std::function<void(std::string, bool)> say;
    std::function<void(const std::string&)> log;
    std::function<bool(uint64_t, std::string&, std::string&)> seedInfo;
    uint64_t focusItem = 0; // 虚拟焦点（语义 FOCUS 被宿主 BLOCKED，不可用）
    bool openSeen = false;  // 扫描曾观测到对话框打开（DIALOG_CLOSE 焦点归还用）
};
