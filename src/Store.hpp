// SPDX-License-Identifier: LGPL-3.0-or-later
// 疯狂戴夫的商店浏览+购买支持：主菜单开店、商品价格/是否买得起朗读、翻页与返回按钮命名。
// 商店由宿主以语义控件暴露（widgets.describe 的 type_name 含 "StoreScreen"），商品是
// 它的语义子项（item = 0x800000000 + StoreItem，value = 价格金币）；开店走 tao.navigation
// 的 TAO_NAVIGATION_STORE（排队到安全帧）。
// 购买（2026-10-09）：本构建宿主把商品语义冻结为 DISABLED|BLOCKED（actions=0），
// 语义 invoke 无法购买，按 kZombieAllowedOffset 同款先例绕过：TaoEngineResolveObject
// 解出 StoreScreen* 并校验 vtable 后，直调 exe 内原生函数（RVA 来自宿主 PDB 反汇编，
// 见下方 kStore* 常量）。门禁与原生 MouseDown 真身（0x1409DA360）同序：戴夫对话
// 气泡激活（this+0x28C）时回车 = 原生推进对话；CanInteractWithButtons 未放开
//（开场动画/叠加模态）则忽略；再依次 SoldOut/Unavailable/ComingSoon/CanAfford，
// 全过才调 PurchaseItem——它只做"金币检查 → 弹原生 [BUY_ITEM] 是/否确认框"，
// 真正扣款发生在用户在确认框按"是"之后——不会回车即买。确认框按钮由
// uiNav 聚焦导航（DIALOG_OPEN/CLOSE 事件），关闭后自动回到商店浏览。
// 不做任何内存/字节补丁；只读取 vtable 指针做身份校验、按宿主 ABI 调用原生函数。
#pragma once
#include "HostServices.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
inline uintptr_t GetHostModuleBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}
#else
inline uintptr_t GetHostModuleBase() {
    return 0;
}
#endif

// 宿主导出（Engine.hpp）：语义对象句柄 → 真实对象指针。与 BoardControl.hpp 的
// 声明一致（extern "C" 可重复声明）。
extern "C" void* TaoEngineResolveObject(TaoPluginObjectHandle handle) noexcept;

// ---- 20261005 版 exe 硬编码 RVA（pdbquery + 反汇编确认；换版本需重对）----
// StoreScreen 主 vtable（构造函数 0x1409D6C40 写入 [this]）：解析结果的身份校验，
// 杜绝句柄命名空间不符/换版本时的野指针调用。
static constexpr size_t kStoreVtableRva = 0x10B9E78;
// 以下均为探针包装入口（x64 成员 ABI：rcx=this，edx=StoreItem）。
static constexpr size_t kStoreRvaPurchaseItem      = 0x9E0090; // void(StoreScreen*, int)
static constexpr size_t kStoreRvaCanAffordItem     = 0x9DD260; // bool(StoreScreen*, int)
static constexpr size_t kStoreRvaIsItemSoldOut     = 0x9DEA60; // bool(StoreScreen*, int)
static constexpr size_t kStoreRvaIsItemUnavailable = 0x9DEC20; // bool(StoreScreen*, int)
static constexpr size_t kStoreRvaIsComingSoon      = 0x9DE860; // bool(StoreScreen*, int)
static constexpr size_t kStoreRvaIsFullVersionOnly = 0x9DEA20; // bool(StoreScreen*, int)
// 无商品参数的门禁/动作（rcx=this）。偏移与顺序来自 MouseDown 真身（0x1409DA360）
// 与 CanInteractWithButtons 真身（0x1409DD55D：+0x260>=0x78 且 +0x28C==0 且
// +0x2A0<=0 且 +0x2B5==0）的反汇编。
static constexpr size_t kStoreRvaCanInteract       = 0x9DD440; // bool CanInteractWithButtons(StoreScreen*)
    static constexpr size_t kStoreRvaAdvanceDave       = 0x9DCE60; // void AdvanceCrazyDaveDialog(StoreScreen*)
    // 退店（2026-10-09 PDB 定论）：ShowStoreScreen 真身（0x140807770）只是
    // new StoreScreen + AddDialog（商店是对话框 id=4，在 mDialogs 里），无阻塞
    // 模态循环；ButtonDepress(100) 真身（0x1409D7E80）只写 Dialog::mResult
    // （+0x22C，构造初值 0x7FFFFFFF）=1000，全二进制无消费者按此关店——直调
    // ButtonDepress / 语义 ACTIVATE"主菜单" / tao.navigation GAME_SELECTOR
    // 请求三条路实测都返回成功但不关店。原生关店入口是 LawnApp::KillStoreScreen
    // （真身 0x1408149E7：GetDialog(4) 非空 → KillDialog(4) + ClearUpdateBacklog），
    // 与鼠标点返回按钮后宿主走的销毁路径一致。StoreScreen+0x238=LawnApp* 回指针
    // （ButtonDepress/Update 真身均以此取 app）。
    static constexpr size_t kStoreRvaKillStoreScreen   = 0x814910; // void KillStoreScreen(LawnApp*)
    static constexpr size_t kStoreOffApp               = 0x238;
// 静态函数：int GetItemCost(StoreItem)（ecx=item，无 this），语义 value 缺失时兜底价格。
static constexpr size_t kStoreRvaGetItemCost       = 0x9DE000;
// StoreScreen+0x28C：戴夫对话气泡激活字节（原生 MouseDown 见它直接转 AdvanceCrazyDaveDialog）。
static constexpr size_t kStoreOffDaveBubble        = 0x28C;
// 原生 MouseDown 对 StoreItem=0x1E 走 [BUY_PVZ] 完整版推销对话框（IsFullVersionOnly 之外的特殊项）。
static constexpr int kStoreItemBuyPvz = 0x1E;
// 防御边界：原生 Is*/Can* 函数按 StoreItem 索引内部表，语义 id 超出此范围不直调。
static constexpr int kStoreItemMax = 64;

class Store {
public:
    explicit Store(const HostServices* host) : host(host) {}
    void SetSay(std::function<void(std::string, bool)> sink) { say = std::move(sink); }
    void SetLog(std::function<void(const std::string&)> sink) { log = std::move(sink); }
    // 弹窗锚点（来自 DIALOG_OPEN 事件）：商店是 Dialog，不在 widgets.root_* 里，
    // 优先用它识别；为空时回退扫描根控件。
    void SetAnchor(TaoPluginAccessibleNodeV2 node) {
        node.item = 0;
        anchor = node;
        if (!node.widget.object_id) {
            if (storeWidget.object_id && IsStoreWidget(storeWidget)) anchor.widget = storeWidget;
            else { storeWidget = {}; active = false; }
        }
    }
    TaoPluginAccessibleNodeV2 Anchor() const { return anchor; }
    bool Browsing() const { return active && SameHandle(anchor.widget, storeWidget); }

    // 宿主商店商品语义子项 id 约定：0x800000000 + StoreItem（StoreItem 有界，见 ConstEnums）。
    static bool IsItemId(uint64_t item) {
        return item >= UINT64_C(0x800000000) && item < UINT64_C(0x800000100);
    }

    // 节流调用（每 ~10 tick）：定位商店控件并维护开关沿与导航按钮命名。
    void Refresh() {
        const bool wasActive = active;
        active = FindStoreWidget();
        if (!active) { storeWidget = {}; namesBuilt = false; interactLogged = false; lastInteractive = false; lastBubble = false; cursor = 0; focused = {}; nativeLogged = false; openAnnounced = false; anchorDiagLogged = false; }
        if (active && (!wasActive || !namesBuilt)) { BuildNavNames(); namesBuilt = true; }
        if (active) {
            // 诊断：可交互状态变化时记一行。优先原生 CanInteractWithButtons
            //（开场计时/戴夫气泡/叠加模态任一未就绪即 false）；原生对象不可用
            // 时退化为宿主义义 actions（本构建恒 0，仅作参考）。
            const bool interactive = NativeCanInteract();
            if (!interactLogged || interactive != lastInteractive) {
                interactLogged = true;
                lastInteractive = interactive;
                Log("商店：可交互=" + std::to_string(interactive));
            }
            const bool bubble = HasBubble();
            if (bubble != lastBubble) {
                lastBubble = bubble;
                if (bubble) Log("商店：检测到戴夫对话气泡，可回车推进");
            }
        }
        if (active && !wasActive) {
            justOpened = !openAnnounced; // DIALOG_OPEN 已播报过就不再重复
            // 刚才是经主菜单某个无名按钮进的店 → 记下它就是商店入口（无需猜坐标）。
            LearnMenuEntry();
        }
        if (menuCandidateTicks > 0) --menuCandidateTicks;
    }

    // 入口按钮位置持久化（档案设置）：学到后跨会话有效，否则每次启动首次键盘
    // 进店都会退回阻塞激活（键盘浏览全废）直到再学一次。
    TaoPluginRect MenuStoreBounds() const { return menuStoreBounds; }
    void SetMenuStoreBounds(TaoPluginRect rect) { if (rect.width > 0) menuStoreBounds = rect; }
    bool ConsumeLearnedDirty() { const bool value = learnedDirty; learnedDirty = false; return value; }

    bool Active() const { return active; }
    // 立即判断锚点（DIALOG_OPEN 事件节点）是否就是商店：看其子项里是否有商品。
    // 用于在弹窗事件里就决定是否让通用控件导航接管（避免抢走宿主焦点）。
    bool AnchorIsStore() {
        if (!anchor.widget.object_id) return false;
        if (!IsStoreWidget(anchor.widget)) return false;
        // 立即生效：DIALOG_OPEN 时刻节流的 Refresh 还没跑到，storeWidget 未设置
        // 会让随后的 AnnounceItems 播"商店未就绪"（2026-10-09 实机日志确认）。
        if (!SameHandle(storeWidget, anchor.widget)) {
            cursor = 0;
            focused = {};
            lastActivated = {};
            nativeLogged = false;
            interactLogged = false;
            lastBubble = false;
            openAnnounced = false;
        }
        storeWidget = anchor.widget;
        active = true;
        BuildNavNames();
        namesBuilt = true;
        // 键盘进店也在这里学习入口按钮：键盘激活会被 exe 的商店模态循环阻塞，
        // 开店期间 OnUpdate 全是嵌套泵帧、Refresh 不运行，等不到它的开关沿学习
        //（2026-10-09 定论：入口学习因此从未生效，"商店"命名/非阻塞进店全失效）。
        LearnMenuEntry();
        return true;
    }
    // 节点是否属于商店对话框子树（对话框自身/商品与气泡语义项/翻页与主菜单按钮）。
    // 键盘进店时宿主会自动聚焦店内原生按钮并发 FOCUS——这类焦点视同"商店内部"，
    // 不交给通用控件导航（否则方向键只能在未命名的翻页按钮间打转，2026-10-09 实测，
    // 鼠标进店无自动聚焦故无此问题）。购买确认框（[BUY_ITEM]）是独立顶层对话框，
    // 其"是/否"按钮不在商店子树内，不受影响。
    bool Owns(const TaoPluginAccessibleNodeV2& node) {
        if (!node.widget.object_id || !storeWidget.object_id) return false;
        if (SameHandle(node.widget, storeWidget)) return true;
        TaoPluginAccessibleNodeV2 root{};
        root.widget = storeWidget;
        TaoPluginAccessibleInfoV2 rootInfo{};
        rootInfo.struct_size = sizeof(rootInfo);
        if (host->ui.describe(host->ui.host_context, root, &rootInfo) != TAO_PLUGIN_OK) return false;
        for (size_t j = 0; j < rootInfo.child_count; ++j) {
            TaoPluginAccessibleNodeV2 child{};
            if (host->ui.child_at(host->ui.host_context, root, j, &child) != TAO_PLUGIN_OK) continue;
            if (SameHandle(child.widget, node.widget)) return true;
        }
        return false;
    }
    // 进店即播报介绍 + 本页商品（DIALOG_OPEN 路径）。设置 openAnnounced 抑制
    // Refresh 开关沿的重复播报（justOpened 只兜底无弹窗事件的进店路径）。
    void AnnounceOpen() {
        openAnnounced = true;
        Say("疯狂戴夫的商店。方向键浏览，F7 朗读本页商品，回车购买，退格退出。", false);
        AnnounceItems();
    }
    // 商店刚打开的一次性标志（由上层播报后清除）。
    bool ConsumeJustOpened() { const bool value = justOpened; justOpened = false; return value; }
    size_t ItemCount() const { return itemCount; }

    bool CanShowStore() const {
        if (!host->navigation.state) return false;
        TaoPluginNavigationStateV1 state{};
        state.struct_size = sizeof(state);
        return host->navigation.state(host->navigation.host_context, &state) == TAO_PLUGIN_OK &&
            (state.flags & TAO_NAVIGATION_CAN_SHOW_STORE) != 0;
    }
    void OpenStore() {
        if (!host->navigation.request) { Say("商店功能不可用"); return; }
        if (!CanShowStore()) { Say("商店尚未解锁"); return; }
        TaoPluginNavigationRequestV1 request{};
        request.struct_size = sizeof(request);
        request.action = TAO_NAVIGATION_STORE;
        if (host->navigation.request(host->navigation.host_context, &request) != TAO_PLUGIN_OK)
            Say("打开商店失败");
    }

    // 商品朗读：名称由宿主给出，这里补价格与是否买得起。
    std::string DescribeItem(const TaoPluginAccessibleNodeV2& node,
                             const TaoPluginAccessibleInfoV2& info, int money) const {
        if (!IsItemId(node.item)) return {};
        const int price = ItemPrice(node.item, info);
        if (price <= 0) return {};
        std::string suffix = "，价格 " + std::to_string(price) + " 金币";
        if (money < price) suffix += "，金币不足";
        return suffix;
    }

    // 商品价格：语义子项 value 优先；缺失/为零时经原生静态 GetItemCost(StoreItem)
    // 兜底（探针包装入口，ecx=item，无 this，不需要 StoreScreen 实例，vtable 未
    // 校验也可用——但仍受 kStoreItemMax 防御边界约束）。
    int ItemPrice(uint64_t nodeItem, const TaoPluginAccessibleInfoV2& info) const {
        const int semantic = int(std::lround(info.value));
        if (semantic > 0) return semantic;
        const int item = int(nodeItem - UINT64_C(0x800000000));
        if (item < 0 || item >= kStoreItemMax) return semantic;
        const auto base = GetHostModuleBase();
        if (!base) return semantic;
        const int native = reinterpret_cast<int (*)(int)>(base + kStoreRvaGetItemCost)(item);
        return native > 0 ? native : semantic;
    }

    // 逐项播报本页商品（名称 + 价格）。直接用 ui.describe 读取，不依赖宿主是否
    // 给出可聚焦/可激活动作——因此即使宿主把商品标为不可交互（actions=0），也能浏览。
    void AnnounceItems() {
        if (!storeWidget.object_id) { Say("商店未就绪"); return; }
        TaoPluginAccessibleNodeV2 root{};
        root.widget = storeWidget;
        TaoPluginAccessibleInfoV2 rootInfo{};
        rootInfo.struct_size = sizeof(rootInfo);
        if (host->ui.describe(host->ui.host_context, root, &rootInfo) != TAO_PLUGIN_OK) { Say("商店读取失败"); return; }
        struct Entry { int x, y; std::string text; };
        std::vector<Entry> entries;
        const int money = Money();
        for (size_t j = 0; j < rootInfo.child_count; ++j) {
            TaoPluginAccessibleNodeV2 child{};
            if (host->ui.child_at(host->ui.host_context, root, j, &child) != TAO_PLUGIN_OK) continue;
            // 纳入所有语义子项：商品（0x800000000+）以及戴夫的对话气泡项（0x600000000，
            // activate = AdvanceCrazyDaveDialog）。后者出现时按回车即可推进对话。
            if (child.item == 0) continue;
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, child, &info) != TAO_PLUGIN_OK) continue;
            std::string line = CopyView(info.name);
            if (line.empty()) line = IsItemId(child.item) ? "未知商品" : "对话";
            const int price = IsItemId(child.item) ? ItemPrice(child.item, info) : 0;
            if (price > 0) {
                line += " " + std::to_string(price) + " 金币";
                if (money >= 0 && money < price) line += "，金币不足";
            }
            entries.push_back({int(info.bounds.x), int(info.bounds.y), std::move(line)});
        }
        if (entries.empty()) { Say("本页没有商品"); return; }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.y != b.y ? a.y < b.y : a.x < b.x; });
        std::string text = "本页 " + std::to_string(entries.size()) + " 件商品：";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i) text += "；";
            text += entries[i].text;
        }
        Say(std::move(text));
    }

    // 仅当宿主把商品标为不可交互（actions=0）时的兜底导航：mod 侧自建商品列表，
    // 方向键在商品与按钮间移动、朗读（价格/是否买得起），回车尝试购买。
    void Arrow(int dx, int dy) {
        auto list = Targets();
        if (list.empty()) { Say("商店没有可浏览的商品"); return; }
        const int dir = (dx + dy) >= 0 ? 1 : -1;
        SyncCursor(list);
        cursor = focused.widget.object_id ? (cursor + dir + int(list.size())) % int(list.size()) :
            (dir > 0 ? 0 : int(list.size()) - 1);
        focused = list[cursor];
        Read(list[cursor], int(cursor), int(list.size()));
    }
    void NextButton(int direction) {
        auto list = Targets();
        std::vector<TaoPluginAccessibleNodeV2> buttons;
        for (const auto& node : list) {
            if (node.item) continue;
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, node, &info) == TAO_PLUGIN_OK &&
                info.role == TAO_ACCESSIBLE_ROLE_BUTTON) buttons.push_back(node);
        }
        if (buttons.empty()) { Say("商店没有可浏览的按钮"); return; }
        const auto current = std::find_if(buttons.begin(), buttons.end(), [&](auto node) {
            return SameNode(node, focused);
        });
        int index = current == buttons.end() ? (direction > 0 ? -1 : 0) : int(current - buttons.begin());
        index = (index + direction + int(buttons.size())) % int(buttons.size());
        focused = buttons[index];
        SyncCursor(list);
        Read(focused, index, int(buttons.size()));
    }
    // 退格出店：对话语义 CANCEL 优先，宿主不支持时激活店内"主菜单"按钮。
    //（uiNav.CancelDialog 需从焦点控件取 info.dialog，商店浏览期间 mod 不持有
    // 焦点，describe 空焦点直接失败——2026-10-09 实测退格被静默吞掉。）
    void Cancel() {
        if (!storeWidget.object_id) return;
        {
            TaoPluginAccessibleNodeV2 self{};
            self.widget = storeWidget;
            invokeguard::Guard invokeGuard;
            const auto status = host->ui.invoke(host->ui.host_context, self,
                TAO_ACCESSIBLE_ACTION_CANCEL, 0, {});
            Log("商店取消 status=" + std::to_string(int(status)));
            if (status == TAO_PLUGIN_OK) return;
        }
        BackToMainMenu();
    }
    void ActivateFocused() {
        auto list = Targets();
        if (list.empty()) return;
        SyncCursor(list);
        focused = list[cursor];
        const TaoPluginAccessibleNodeV2 node = list[cursor];
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        // 陈旧目标守卫：返回/主菜单按钮激活后商店弹窗即关闭，控件销毁；对已销毁
        // 控件 invoke 会被宿主二次 ButtonPress 崩溃（同 UiNavigation 重复激活守卫）。
        if (host->ui.describe(host->ui.host_context, node, &info) != TAO_PLUGIN_OK) {
            Say("此控件已失效");
            return;
        }
        if (SameNode(node, lastActivated) &&
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - lastActivatedAt).count() < 400.0) {
            Log("商店激活 skip: 同一目标 400ms 内重复激活");
            return;
        }
        // 商品：宿主语义冻结（DISABLED|BLOCKED），走原生 PurchaseItem（弹是/否确认框）。
        if (IsItemId(node.item)) { TryPurchase(node, info); return; }
        // "主菜单"（返回）按钮：语义 ACTIVATE 对它无效（翻页按钮却有效，2026-10-09
        // 实测 bubble=0/canInteract=1 下 status=0 但不关店），改走导航服务退店。
        std::string name = CopyView(info.name);
        if (name.empty()) name = LookupButtonName(info);
        if (node.item == 0 && (name == "返回" || name.find("主菜单") != std::string::npos)) {
            BackToMainMenu();
            return;
        }
        // 按钮 / 戴夫对话气泡：语义 invoke 原路径。
        invokeguard::Guard invokeGuard; // 阻塞型激活在栈标志：期间按键放行原生（见 HostServices.hpp）
        const auto status = host->ui.invoke(host->ui.host_context, node,
            TAO_ACCESSIBLE_ACTION_ACTIVATE, 0, {});
        Log("商店激活 status=" + std::to_string(int(status)));
        if (status == TAO_PLUGIN_OK) {
            lastActivated = node;
            lastActivatedAt = std::chrono::steady_clock::now();
            return;
        }
        Say(info.role == TAO_ACCESSIBLE_ROLE_BUTTON ? "此按钮当前不可用" : "此商品当前不可购买");
    }

    // 原生购买：门禁与原生 StoreScreen::MouseDown 同序（IsFullVersionOnly/BUY_PVZ →
    // SoldOut → Unavailable → ComingSoon → CanAfford），全部通过才调 PurchaseItem——
    // 它只弹原生 [BUY_ITEM] 是/否确认框，确认后才真正扣款。确认框的 DIALOG_OPEN
    // 会播报正文并由 uiNav 聚焦"是"按钮，关闭时焦点自动清空、回到商店浏览。
    void TryPurchase(const TaoPluginAccessibleNodeV2& node, const TaoPluginAccessibleInfoV2& info) {
        (void)info;
        const int item = int(node.item - UINT64_C(0x800000000));
        void* storeScreen = NativeStoreScreen();
        if (storeScreen && item >= 0 && item < kStoreItemMax) {
            const auto base = GetHostModuleBase();
            if (!base) return;
            const auto gate = [&](size_t rva) {
                return reinterpret_cast<bool (*)(void*, int)>(base + rva)(storeScreen, item);
            };
            // 与原生 MouseDown 同序的前置门禁：气泡在 → 点击只是推进对话；
            // 开场计时/叠加模态未就绪（CanInteractWithButtons=false）→ 点击被吞。
            if (*reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(storeScreen) + kStoreOffDaveBubble)) {
                reinterpret_cast<void (*)(void*)>(base + kStoreRvaAdvanceDave)(storeScreen);
                Log("商店购买：戴夫对话气泡激活，已原生推进对话（等效点击）");
                return;
            }
            if (!reinterpret_cast<bool (*)(void*)>(base + kStoreRvaCanInteract)(storeScreen)) {
                Say("商店还没准备好");
                Log("商店购买：CanInteractWithButtons=false（开场/模态未就绪），点击被原生吞掉");
                return;
            }
            if (item == kStoreItemBuyPvz || gate(kStoreRvaIsFullVersionOnly)) { Say("此物品需要购买完整版"); return; }
            if (gate(kStoreRvaIsItemSoldOut)) { Say("此物品已售完"); return; }
            if (gate(kStoreRvaIsItemUnavailable)) { Say("此物品当前不可用"); return; }
            if (gate(kStoreRvaIsComingSoon)) { Say("此物品敬请期待"); return; }
            if (!gate(kStoreRvaCanAffordItem)) { Say("金币不足"); return; }
            // 记入 400ms 守卫：确认框出现前的回车连按不再重复触发（重复调用会先
            // 关闭旧确认框再弹新框，虽无害但语音会被反复打断）。
            lastActivated = node;
            lastActivatedAt = std::chrono::steady_clock::now();
            reinterpret_cast<void (*)(void*, int)>(base + kStoreRvaPurchaseItem)(storeScreen, item);
            Log("商店购买：已调原生 PurchaseItem item=" + std::to_string(item) +
                "，等待 [BUY_ITEM] 确认框（是=购买，否=取消）");
            return;
        }
        // 原生链路不可用（解析失败/vtable 不符/越界）：保留旧的语义激活路径，
        // 宿主 DISABLED 门禁会拒绝并给出原反馈，不引入新行为。
        Log("商店购买：原生对象不可用（resolve/vtable/越界 item=" + std::to_string(item) + "），回退语义激活");
        invokeguard::Guard invokeGuard;
        const auto status = host->ui.invoke(host->ui.host_context, node,
            TAO_ACCESSIBLE_ACTION_ACTIVATE, 0, {});
        Log("商店激活 status=" + std::to_string(int(status)));
        if (status == TAO_PLUGIN_OK) {
            lastActivated = node;
            lastActivatedAt = std::chrono::steady_clock::now();
            return;
        }
        Say("此商品当前不可购买");
    }

    // 可交互判定：优先原生 CanInteractWithButtons（与 MouseDown 同一门禁，
    // 开场计时 >=0x78、无戴夫气泡、无叠加模态才为 true）；原生对象不可用时
    // 退化为宿主义务层 actions（本构建恒 0）。
    bool NativeCanInteract() {
        void* storeScreen = NativeStoreScreen();
        if (!storeScreen) return ItemsInteractive();
        const auto base = GetHostModuleBase();
        if (!base) return ItemsInteractive();
        return reinterpret_cast<bool (*)(void*)>(base + kStoreRvaCanInteract)(storeScreen);
    }
    // 商品当前是否可交互（宿主给出非零 actions）。用于判断商店控件是否已获得焦点，
    // 以及上层是否改用 mod 侧的兜底导航。
    bool ItemsInteractive() {
        if (!storeWidget.object_id) return false;
        TaoPluginAccessibleNodeV2 root{};
        root.widget = storeWidget;
        TaoPluginAccessibleInfoV2 rootInfo{};
        rootInfo.struct_size = sizeof(rootInfo);
        if (host->ui.describe(host->ui.host_context, root, &rootInfo) != TAO_PLUGIN_OK) return false;
        for (size_t j = 0; j < rootInfo.child_count; ++j) {
            TaoPluginAccessibleNodeV2 child{};
            if (host->ui.child_at(host->ui.host_context, root, j, &child) != TAO_PLUGIN_OK) continue;
            if (!IsItemId(child.item)) continue;
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, child, &info) != TAO_PLUGIN_OK) continue;
            return info.actions != 0;
        }
        return false;
    }

    // 商店内无名按钮（上一页/下一页/返回）命名。
    std::string LookupButtonName(const TaoPluginAccessibleInfoV2& info) const {
        if (!active) return {};
        for (const auto& entry : navNames)
            if (Near(entry.first, info.bounds)) return entry.second;
        return {};
    }
    // 主菜单商店入口命名（首次经该按钮进店后学习，避免硬编码坐标）。
    std::string LookupMenuButton(const TaoPluginAccessibleInfoV2& info) const {
        if (menuStoreBounds.width <= 0) return {};
        return Near(menuStoreBounds, info.bounds) ? std::string("商店") : std::string{};
    }

    // 激活前回调：记录主菜单候选；翻页按钮被激活时播报方向。
    void NoteActivation(const TaoPluginAccessibleNodeV2& node, const TaoPluginAccessibleInfoV2& info,
                        const std::string& name) {
        if (!active && info.scene == int(tao::enums::GameScenes::SCENE_MENU.Value()) &&
            info.role == TAO_ACCESSIBLE_ROLE_BUTTON && name.empty() && node.item == 0) {
            menuCandidateBounds = info.bounds;
            menuCandidateTicks = 12; // ~120 tick 窗口（Refresh 每 ~10 tick 衰减一次）
        }
        if (name == "上一页") Say("上一页");
        else if (name == "下一页") Say("下一页");
    }

private:
    void SyncCursor(const std::vector<TaoPluginAccessibleNodeV2>& list) {
        const auto current = std::find_if(list.begin(), list.end(), [&](auto node) {
            return SameNode(node, focused);
        });
        cursor = current == list.end() ? 0 : int(current - list.begin());
        if (current == list.end()) focused = {};
    }
    // 原生关店：与 MouseDown 同门禁——戴夫气泡在时点击只是推进对话（返回 true
    // 但未退店，用户再按一次）；气泡落时直调 LawnApp::KillStoreScreen（经
    // StoreScreen+0x238 的 app 回指针），这是鼠标点返回按钮后宿主销毁商店的
    // 同一入口。对象经 vtable 校验，杜绝商店已拆除时误调。
    bool NativePressBack() {
        void* storeScreen = NativeStoreScreen();
        if (!storeScreen) return false;
        const auto base = GetHostModuleBase();
        if (!base) return false;
        if (*reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(storeScreen) + kStoreOffDaveBubble)) {
            reinterpret_cast<void (*)(void*)>(base + kStoreRvaAdvanceDave)(storeScreen);
            Log("商店退店：戴夫对话气泡在，已原生推进对话（等效点击），再按一次退出");
            return true;
        }
        void* app = *reinterpret_cast<void**>(static_cast<uint8_t*>(storeScreen) + kStoreOffApp);
        reinterpret_cast<void (*)(void*)>(base + kStoreRvaKillStoreScreen)(app);
        Log("商店退店：已调原生 LawnApp::KillStoreScreen");
        return true;
    }
    // 退店回主菜单：首选原生 KillStoreScreen（理由见 kStoreRvaKillStoreScreen
    // 注释，导航 GAME_SELECTOR 请求与语义 ACTIVATE 实测均假成功）；原生对象
    // 不可用才回退 tao.navigation 请求。
    void BackToMainMenu() {
        if (NativePressBack()) return;
        if (host->navigation.request) {
            TaoPluginNavigationRequestV1 request{};
            request.struct_size = sizeof(request);
            request.action = TAO_NAVIGATION_GAME_SELECTOR;
            const auto status = host->navigation.request(host->navigation.host_context, &request);
            Log("商店退店：原生不可用，导航 GAME_SELECTOR status=" + std::to_string(int(status)));
            if (status == TAO_PLUGIN_OK) return;
        }
        Say("无法退出商店");
    }
    // "键盘激活主菜单无名按钮 → 随后商店打开"的因果相关学习：记下按钮位置，
    // 之后 LookupMenuButton 命名它为"商店"，SetBeforeActivate 据此改走
    // tao.navigation 非阻塞进店。learnedDirty 通知上层写入档案设置持久化。
    void LearnMenuEntry() {
        if (menuCandidateTicks <= 0) return;
        menuStoreBounds = menuCandidateBounds;
        menuCandidateTicks = 0;
        learnedDirty = true;
        Log("商店入口按钮已学习：x=" + std::to_string(int(menuStoreBounds.x)) +
            " y=" + std::to_string(int(menuStoreBounds.y)) +
            " w=" + std::to_string(int(menuStoreBounds.width)) +
            " h=" + std::to_string(int(menuStoreBounds.height)));
    }
    // ---- 原生购买链路辅助 ----
    // 解析商店控件句柄并校验身份（vtable 指针与 PDB 确认的 StoreScreen 主表比对）。
    // 失败返回空——任何原生调用前必须通过本校验，杜绝句柄命名空间不符/换版本时
    // 的野指针调用。每次进店首个购买尝试记一行诊断日志。
    void* NativeStoreScreen() {
        if (!storeWidget.object_id) return nullptr;
        void* object = TaoEngineResolveObject(storeWidget);
        const uintptr_t base = GetHostModuleBase();
        const uintptr_t vtable = object ? *reinterpret_cast<const uintptr_t*>(object) : 0;
        const bool ok = object && base && vtable == base + kStoreVtableRva;
        if (!nativeLogged) {
            nativeLogged = true;
            Log("商店原生对象：resolve=" + std::string(object ? "非空" : "空") +
                " vtable=" + std::to_string(vtable) + " 期望=" + std::to_string(base + kStoreVtableRva) +
                (ok ? " 校验通过，原生购买可用" : " 校验失败，购买退化为只读"));
        }
        return ok ? object : nullptr;
    }
    // 当前金币（a11y context 与 DescribeItem 同源）。读取失败返回 -1（不做金币判断）。
    int Money() const {
        TaoPluginAccessibilityContextV1 context{};
        context.struct_size = sizeof(context);
        if (!host->a11y.context || host->a11y.context(host->a11y.host_context, &context) != TAO_PLUGIN_OK)
            return -1;
        return context.money;
    }

    // 恢复读档的棋盘在控件树里是一个 Board 控件，ui.describe 它会调 BoardActionGate
    // 崩（老虎机续玩对话框同族缺陷，见 UiNavigation 的 skipBoard）；识别用
    // widgets.describe 的 type_name，该接口安全。
    bool IsBoardWidget(TaoPluginObjectHandle handle) {
        TaoPluginWidgetInfoV1 w{};
        w.struct_size = sizeof(w);
        return host->widgets.describe(host->widgets.host_context, handle, &w) == TAO_PLUGIN_OK &&
            CopyView(w.type_name).find("Board") != std::string::npos;
    }
    bool FindStoreWidget() {
        // 已识别的商店控件优先复用：叠在上面的弹窗（[BUY_ITEM] 购买确认框）开关
        // 会把 anchor 换成弹窗节点，确认框关闭后 anchor 失效（弹窗已销毁、商店是
        // Dialog 不在 widgets.root_* 里）——不复用 storeWidget 会在首次购买后丢失
        // 商店识别（active=false，浏览/购买全断）。
        if (storeWidget.object_id && IsStoreWidget(storeWidget)) return true;
        if (anchor.widget.object_id && IsStoreWidget(anchor.widget)) { storeWidget = anchor.widget; return true; }
        // 曾识别为商店的锚点突然认不出商品子项（弹窗被宿主重建/语义子项变化）：
        // 记一行诊断再回退全树扫描。非商店弹窗（如"点击开始"）不记——它们本来
        // 就没有商品子项（2026-10-09 实测：诊断在非商店弹窗上每刷新周期刷屏）。
        if (anchor.widget.object_id && storeWidget.object_id && !anchorDiagLogged) {
            anchorDiagLogged = true;
            Log("商店诊断：锚点 id=" + std::to_string(anchor.widget.object_id) +
                " 未识别出商品子项，回退全树扫描");
        }
        size_t count = 0;
        if (host->widgets.root_count(host->widgets.host_context, &count) != TAO_PLUGIN_OK) return false;
        for (size_t i = 0; i < count && i < 1024; ++i) {
            TaoPluginObjectHandle handle{};
            if (host->widgets.root_at(host->widgets.host_context, i, &handle) != TAO_PLUGIN_OK) continue;
            if (IsBoardWidget(handle)) continue; // 棋盘控件整棵跳过，不做 ui.describe
            if (IsStoreWidget(handle)) { storeWidget = handle; return true; }
        }
        return false;
    }
    // 兜底导航目标列表：先商品（按 行/列 排序），后按钮（按 x 排序）。
    std::vector<TaoPluginAccessibleNodeV2> Targets() {
        std::vector<std::pair<TaoPluginRect, TaoPluginAccessibleNodeV2>> items, buttons;
        TaoPluginAccessibleNodeV2 root{};
        root.widget = storeWidget;
        TaoPluginAccessibleInfoV2 rootInfo{};
        rootInfo.struct_size = sizeof(rootInfo);
        if (host->ui.describe(host->ui.host_context, root, &rootInfo) != TAO_PLUGIN_OK) return {};
        for (size_t j = 0; j < rootInfo.child_count; ++j) {
            TaoPluginAccessibleNodeV2 child{};
            if (host->ui.child_at(host->ui.host_context, root, j, &child) != TAO_PLUGIN_OK) continue;
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, child, &info) != TAO_PLUGIN_OK) continue;
            // 商品（0x800000000+）与其它语义项（如戴夫对话气泡 0x600000000）都纳入浏览列表，
            // 否则用户无法推进戴夫的对话、商店就一直处于不可交互状态。
            if (child.item != 0) items.emplace_back(info.bounds, child);
            else if (info.role == TAO_ACCESSIBLE_ROLE_BUTTON && info.bounds.width > 0)
                buttons.emplace_back(info.bounds, child);
        }
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            return a.first.y != b.first.y ? a.first.y < b.first.y : a.first.x < b.first.x; });
        std::sort(buttons.begin(), buttons.end(), [](const auto& a, const auto& b) { return a.first.x < b.first.x; });
        std::vector<TaoPluginAccessibleNodeV2> list;
        for (auto& e : items) list.push_back(e.second);
        for (auto& e : buttons) list.push_back(e.second);
        return list;
    }
    void Read(const TaoPluginAccessibleNodeV2& node, int index, int total) {
        TaoPluginAccessibleInfoV2 info{};
        info.struct_size = sizeof(info);
        if (host->ui.describe(host->ui.host_context, node, &info) != TAO_PLUGIN_OK) { Say("读取失败"); return; }
        std::string name = CopyView(info.name);
        if (name.empty()) name = LookupButtonName(info);
        if (name.empty()) name = "未命名控件";
        std::string text = std::to_string(index + 1) + "/" + std::to_string(total) + "，" + name;
        if (IsItemId(node.item)) {
            const int price = ItemPrice(node.item, info);
            if (price > 0) {
                text += "，价格 " + std::to_string(price) + " 金币";
                const int money = Money();
                if (money >= 0 && money < price) text += "，金币不足";
            }
        }
        // 宿主把商品统一标 DISABLED（本构建限制，实际可经原生链路购买），不再播"不可购买"。
        Say(std::move(text));
    }

    // 是否存在戴夫"点击继续"气泡项（id = 0x600000000）。
    bool HasBubble() {
        if (!storeWidget.object_id) return false;
        TaoPluginAccessibleNodeV2 root{};
        root.widget = storeWidget;
        TaoPluginAccessibleInfoV2 rootInfo{};
        rootInfo.struct_size = sizeof(rootInfo);
        if (host->ui.describe(host->ui.host_context, root, &rootInfo) != TAO_PLUGIN_OK) return false;
        for (size_t j = 0; j < rootInfo.child_count; ++j) {
            TaoPluginAccessibleNodeV2 child{};
            if (host->ui.child_at(host->ui.host_context, root, j, &child) != TAO_PLUGIN_OK) continue;
            if (child.item == UINT64_C(0x600000000)) return true;
        }
        return false;
    }

    // 商店识别：宿主把 StoreScreen 的 widgets.describe 类型上报为 "Dialog"（不是
    // "StoreScreen"），故改为看子项里是否有商店商品语义子项（item = 0x800000000+）。
    bool IsStoreWidget(TaoPluginObjectHandle handle) {
        TaoPluginAccessibleNodeV2 root{};
        root.widget = handle;
        TaoPluginAccessibleInfoV2 rootInfo{};
        rootInfo.struct_size = sizeof(rootInfo);
        if (host->ui.describe(host->ui.host_context, root, &rootInfo) != TAO_PLUGIN_OK) return false;
        for (size_t j = 0; j < rootInfo.child_count; ++j) {
            TaoPluginAccessibleNodeV2 child{};
            if (host->ui.child_at(host->ui.host_context, root, j, &child) != TAO_PLUGIN_OK) continue;
            if (IsItemId(child.item) || child.item == UINT64_C(0x600000000)) return true;
        }
        return false;
    }

    // 枚举商店控件子项：收集无名 BUTTON 的 bounds 作为翻页/返回按钮，按位置命名
    //（最左=上一页，最右=下一页，其余=返回）。实机日志便于校正。
    void BuildNavNames() {
        navNames.clear();
        itemCount = 0;
        TaoPluginAccessibleNodeV2 root{};
        root.widget = storeWidget;
        TaoPluginAccessibleInfoV2 rootInfo{};
        rootInfo.struct_size = sizeof(rootInfo);
        if (host->ui.describe(host->ui.host_context, root, &rootInfo) != TAO_PLUGIN_OK) return;
        std::vector<TaoPluginRect> buttons;
        for (size_t j = 0; j < rootInfo.child_count; ++j) {
            TaoPluginAccessibleNodeV2 child{};
            if (host->ui.child_at(host->ui.host_context, root, j, &child) != TAO_PLUGIN_OK) continue;
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, child, &info) != TAO_PLUGIN_OK) continue;
            if (IsItemId(child.item)) { ++itemCount; continue; }
            if (child.item == 0 && info.role == TAO_ACCESSIBLE_ROLE_BUTTON && CopyView(info.name).empty() &&
                info.bounds.width > 0 && info.bounds.height > 0)
                buttons.push_back(info.bounds);
        }
        std::sort(buttons.begin(), buttons.end(),
            [](const TaoPluginRect& a, const TaoPluginRect& b) { return a.x < b.x; });
        if (buttons.size() >= 2) {
            navNames.emplace_back(buttons.front(), "上一页");
            navNames.emplace_back(buttons.back(), "下一页");
            for (size_t k = 1; k + 1 < buttons.size(); ++k) navNames.emplace_back(buttons[k], "返回");
        } else if (buttons.size() == 1) {
            navNames.emplace_back(buttons.front(), "返回");
        }
        for (const auto& entry : navNames)
            Log("商店按钮命名：" + entry.second + " bounds=(" + std::to_string(int(entry.first.x)) + "," +
                std::to_string(int(entry.first.y)) + "," + std::to_string(int(entry.first.width)) + "," +
                std::to_string(int(entry.first.height)) + ")");
        Log("商店打开：本页商品数 " + std::to_string(itemCount));
    }

    static bool Near(const TaoPluginRect& a, const TaoPluginRect& b) {
        return std::abs(int(a.x) - int(b.x)) <= 4 && std::abs(int(a.y) - int(b.y)) <= 4 &&
            std::abs(int(a.width) - int(b.width)) <= 4 && std::abs(int(a.height) - int(b.height)) <= 4;
    }
    void Say(std::string text, bool interrupt = true) const { if (say) say(std::move(text), interrupt); }
    void Log(const std::string& text) const { if (log) log(text); }

    const HostServices* host;
    int cursor = 0;
    TaoPluginAccessibleNodeV2 focused{};
    std::function<void(std::string, bool)> say;
    std::function<void(const std::string&)> log;
    TaoPluginObjectHandle storeWidget{};
    TaoPluginAccessibleNodeV2 anchor{};
    TaoPluginAccessibleNodeV2 lastActivated{}; // 重复激活守卫：上次成功激活的目标
    std::chrono::steady_clock::time_point lastActivatedAt{}; // 及其时间（400ms 防双击窗口）
    bool active = false, justOpened = false, namesBuilt = false, interactLogged = false, lastInteractive = false, lastBubble = false, nativeLogged = false;
    bool openAnnounced = false;    // DIALOG_OPEN 已播报过进店介绍（抑制 justOpened 重复）
    bool anchorDiagLogged = false; // 锚点失效诊断只记一次
    size_t itemCount = 0;
    std::vector<std::pair<TaoPluginRect, std::string>> navNames;
    TaoPluginRect menuCandidateBounds{};
    int menuCandidateTicks = 0;
    TaoPluginRect menuStoreBounds{};
    bool learnedDirty = false;       // 入口按钮位置新学到，待写入档案设置
};
