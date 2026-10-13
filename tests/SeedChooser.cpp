// SPDX-License-Identifier: LGPL-3.0-or-later
#include "SeedChooser.hpp"
#include <iostream>
#include <stdexcept>

bool A11ySeedChooserAnimating() { return false; }
bool A11ySeedChooserAvailable() { return false; }

struct FakeChooser {
    static constexpr uint64_t kCard = UINT64_C(0x200000000);
    static constexpr uint64_t kMenu = UINT64_C(0x100000064);
    static constexpr uint64_t kStart = UINT64_C(0x100000065);
    HostServices host{};
    UiNavigation nav{&host};
    SeedChooser chooser{&host, [this] { return flying; }, [this] { return scene == 2 && panelReady; }};
    bool flying = false, hideMoving = false, modal = false, full = false;
    bool panelReady = true;
    int scene = 2, moving = -1, generation = 1;
    uint64_t actualFocus = 0;
    bool selected[4]{};
    std::vector<uint64_t> focusCalls;
    std::vector<std::string> speeches;
    int rootScans = 0;
    TaoPluginAccessibleNodeV2 Node(uint64_t item = 0) const {
        TaoPluginAccessibleNodeV2 node{};
        node.widget.struct_size = sizeof(node.widget);
        node.widget.object_id = 2;
        node.widget.generation = generation;
        node.widget.world_generation = 1;
        node.item = item;
        return node;
    }
    FakeChooser() {
        host.ui.host_context = host.widgets.host_context = host.a11y.host_context = this;
        host.a11y.context = [](void* ctx, TaoPluginAccessibilityContextV1* out) {
            auto& s = *static_cast<FakeChooser*>(ctx);
            out->scene = s.scene; out->flags = s.modal ? TAO_INPUT_MODAL : 0;
            return TAO_PLUGIN_OK;
        };
        host.widgets.root_count = [](void* ctx, size_t* out) {
            ++static_cast<FakeChooser*>(ctx)->rootScans; *out = 1; return TAO_PLUGIN_OK;
        };
        host.widgets.root_at = [](void* ctx, size_t, TaoPluginObjectHandle* out) {
            *out = static_cast<FakeChooser*>(ctx)->Node().widget; return TAO_PLUGIN_OK;
        };
        host.ui.child_at = [](void* ctx, TaoPluginAccessibleNodeV2, size_t index, TaoPluginAccessibleNodeV2* out) {
            auto& s = *static_cast<FakeChooser*>(ctx);
            *out = s.Node(index == 0 ? kMenu : index == 5 ? kStart : kCard + index - 1);
            return TAO_PLUGIN_OK;
        };
        host.ui.describe = [](void* ctx, TaoPluginAccessibleNodeV2 node, TaoPluginAccessibleInfoV2* out) {
            auto& s = *static_cast<FakeChooser*>(ctx);
            if (node.widget.generation != s.generation ||
                (s.hideMoving && s.flying && node.item == kCard + s.moving)) return TAO_PLUGIN_STALE_HANDLE;
            *out = {}; out->struct_size = sizeof(*out); out->node = node;
            out->scene = s.scene; out->flags = TAO_WIDGET_VISIBLE;
            if (node.item == s.actualFocus) out->flags |= TAO_WIDGET_FOCUSED;
            if (node.item == 0) {
                out->role = TAO_ACCESSIBLE_ROLE_GENERIC; out->child_count = 6; out->bounds = {0,0,800,600};
                return TAO_PLUGIN_OK;
            }
            bool enabled = true;
            if (SeedChooser::IsCardItem(node.item)) {
                const int seed = int(node.item - kCard);
                static constexpr const char* names[] = {"豌豆射手", "向日葵", "樱桃炸弹", "禁用植物"};
                out->name = tao::AbiString(names[seed]); out->role = TAO_ACCESSIBLE_ROLE_ITEM;
                out->bounds = {22 + seed * 53,123,50,70};
                if (s.selected[seed]) { out->flags |= TAO_ACCESSIBLE_SELECTED; out->bounds = {seed * 53,8,50,70}; }
                if (s.flying && seed == s.moving) out->bounds = {35,70,50,70};
                // 与真实宿主一致：任意卡飞行时，所有卡都 DISABLED、actions=0。
                enabled = !s.flying && seed != 3 && (!s.full || s.selected[seed]);
            } else {
                out->role = TAO_ACCESSIBLE_ROLE_BUTTON;
                out->name = tao::AbiString(node.item == kMenu ? "菜单" : "一起摇滚吧！");
                out->bounds = node.item == kMenu ? TaoPluginRect{600,10,100,40} : TaoPluginRect{600,500,100,40};
                enabled = node.item == kMenu || s.full;
            }
            if (enabled) out->actions = TAO_ACCESSIBLE_ACTION_FOCUS | TAO_ACCESSIBLE_ACTION_ACTIVATE;
            else out->flags |= TAO_WIDGET_DISABLED;
            if (s.modal) { out->flags |= TAO_ACCESSIBLE_BLOCKED; out->actions = 0; }
            return TAO_PLUGIN_OK;
        };
        host.ui.invoke = [](void* ctx, TaoPluginAccessibleNodeV2 node, uint32_t action, double, TaoPluginStringView) {
            auto& s = *static_cast<FakeChooser*>(ctx);
            TaoPluginAccessibleInfoV2 info{};
            if (s.host.ui.describe(ctx, node, &info) != TAO_PLUGIN_OK || !(info.actions & action))
                return TAO_PLUGIN_NOT_AVAILABLE;
            if (action == TAO_ACCESSIBLE_ACTION_FOCUS) {
                s.actualFocus = node.item; s.focusCalls.push_back(node.item);
            } else if (action == TAO_ACCESSIBLE_ACTION_ACTIVATE && SeedChooser::IsCardItem(node.item)) {
                s.moving = int(node.item - kCard);
                s.selected[s.moving] = !s.selected[s.moving]; s.flying = true;
                s.actualFocus = kMenu; // 模拟宿主在全卡禁用后暂时退到菜单。
            }
            return TAO_PLUGIN_OK;
        };
        nav.SetOnFocus([this] { chooser.NoteFocus(nav.Focused()); });
        const auto say = [this](std::string text, bool) { speeches.push_back(std::move(text)); };
        nav.SetSay(say); chooser.SetSay(say); chooser.SetUi(&nav);
    }
    void PickPea() {
        nav.Focus(Node(kCard));
        chooser.ActivateFocused();
    }
};

int main() {
    int checks = 0;
    const auto require = [&](bool ok, const char* why) { if (!ok) throw std::runtime_error(why); ++checks; };
    try {
        FakeChooser s; s.PickPea();
        require(s.flying, "activation starts flight");
        require(s.chooser.IgnoreHostFocus(s.Node(FakeChooser::kMenu)), "automatic menu focus must be ignored");
        const int scansBeforeArrows = s.rootScans;
        s.nav.SyncFocus(s.Node(FakeChooser::kMenu)); // 即使物理焦点已迁移，也按逻辑卡片起点导航。
        s.chooser.Arrow(1,0);
        require(s.nav.Focused().item == FakeChooser::kCard + 1, "immediate Right from pea must land on sunflower");
        require(s.speeches.back() == "向日葵", "deferred focus must announce its logical destination immediately");
        s.chooser.Arrow(1,0);
        require(s.nav.Focused().item == FakeChooser::kCard + 2, "second fast Right advances to cherry");
        s.chooser.Arrow(1,0);
        require(s.nav.Focused().item == FakeChooser::kCard + 2, "permanently disabled card and menu must be skipped in flight");
        s.chooser.Arrow(-1,0);
        require(s.rootScans == scansBeforeArrows, "rapid arrows during flight must reuse the stable grid");
        const int scansInFlight = s.rootScans;
        for (int i = 0; i < 8; ++i) s.chooser.Pump();
        require(s.rootScans == scansInFlight, "flight retries must not rescan the full chooser tree");
        require(s.nav.Focused().item == FakeChooser::kCard + 1, "retry must not steal focus back to pea");
        s.flying = false;
        for (int i = 0; i < 4; ++i) s.chooser.Pump();
        require(s.actualFocus == FakeChooser::kCard + 1, "host focus must eventually synchronize to sunflower");
        require(std::find(s.focusCalls.begin(),s.focusCalls.end(),FakeChooser::kMenu) == s.focusCalls.end(),
            "automatic retries must never focus menu");

        FakeChooser missing; missing.PickPea(); missing.hideMoving = true; ++missing.generation;
        missing.chooser.Arrow(1,0);
        require(missing.nav.Focused().item == FakeChooser::kCard + 1, "missing/recreated source must retain its pinned origin");
        missing.flying = false;
        for (int i = 0; i < 4; ++i) missing.chooser.Pump();
        require(missing.actualFocus == FakeChooser::kCard + 1, "recreated node must be re-resolved");

        FakeChooser modal; modal.PickPea(); modal.modal = true; modal.flying = false;
        for (int i = 0; i < 4; ++i) modal.chooser.Pump();
        require(modal.actualFocus == FakeChooser::kMenu, "modal must suspend card retry");
        modal.modal = false; modal.nav.Focus(modal.Node(FakeChooser::kMenu));
        for (int i = 0; i < 4; ++i) modal.chooser.Pump();
        require(modal.nav.Focused().item == FakeChooser::kMenu, "explicit Tab/button focus must cancel old card retry");

        FakeChooser full; full.PickPea(); full.full = true; full.flying = false;
        for (int i = 0; i < 4; ++i) full.chooser.Pump();
        require(full.actualFocus == FakeChooser::kStart, "full bank must still transfer focus to start");
        FakeChooser leave; leave.PickPea(); leave.scene = 3; leave.chooser.Pump(); leave.flying = false;
        for (int i = 0; i < 4; ++i) leave.chooser.Pump();
        require(leave.actualFocus == FakeChooser::kMenu, "leaving chooser must discard pending retry");
        FakeChooser mouse; mouse.chooser.Pump(); // 鼠标选卡之前的被动预采样。
        mouse.flying = true; mouse.moving = 0;
        mouse.chooser.ObserveHostFocus(mouse.Node(FakeChooser::kCard));
        mouse.nav.SyncFocus(mouse.Node(FakeChooser::kCard));
        mouse.chooser.Arrow(1,0);
        require(mouse.nav.Focused().item == FakeChooser::kCard + 1, "mouse activation also preserves the card grid origin");
        for (int i = 0; i < 150; ++i) mouse.chooser.Pump();
        mouse.flying = false;
        for (int i = 0; i < 4; ++i) mouse.chooser.Pump();
        require(mouse.actualFocus == FakeChooser::kCard + 1, "long flight must not expire the logical destination before focus becomes available");
        FakeChooser loading; loading.panelReady = false; loading.chooser.Pump();
        require(loading.rootScans == 0, "sliding panel/Dave dialogue must not prime a disabled grid");
        loading.panelReady = true; loading.chooser.Pump();
        loading.flying = true; loading.moving = 0;
        loading.nav.SyncFocus(loading.Node(FakeChooser::kCard));
        loading.chooser.ObserveHostFocus(loading.Node(FakeChooser::kCard)); loading.chooser.Arrow(1,0);
        require(loading.nav.Focused().item == FakeChooser::kCard + 1, "first mouse selection after intro must use ready grid eligibility");
        std::cout << checks << " seed chooser flight navigation checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
