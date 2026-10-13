// SPDX-License-Identifier: LGPL-3.0-or-later
#include "UiNavigation.hpp"
#include <iostream>
#include <stdexcept>

struct DialogHost {
    HostServices host;
    UiNavigation nav{&host};
    uint64_t top = 10;
    int describes = 0;
    std::vector<std::string> spoken;
    static TaoPluginAccessibleNodeV2 Node(uint64_t id) {
        TaoPluginAccessibleNodeV2 n{};
        n.widget.struct_size = sizeof(n.widget);
        n.widget.object_id = id; n.widget.generation = n.widget.world_generation = 1;
        return n;
    }
    DialogHost() {
        host.ui.host_context = host.widgets.host_context = host.a11y.host_context = this;
        host.widgets.at_point = [](void* ctx, int32_t, int32_t, TaoPluginObjectHandle* out) {
            *out = Node(static_cast<DialogHost*>(ctx)->top + 1).widget;
            return TAO_PLUGIN_OK;
        };
        host.widgets.describe = [](void*, TaoPluginObjectHandle h, TaoPluginWidgetInfoV1* out) {
            out->type_name = tao::AbiString(h.object_id % 10 == 0 ? "Dialog" : "Widget");
            return TAO_PLUGIN_OK;
        };
        host.ui.describe = [](void* ctx, TaoPluginAccessibleNodeV2 n, TaoPluginAccessibleInfoV2* out) {
            auto& s = *static_cast<DialogHost*>(ctx); ++s.describes;
            const uint64_t id = n.widget.object_id, dialog = id / 10 * 10;
            out->dialog = Node(dialog).widget;
            out->flags = TAO_WIDGET_VISIBLE;
            out->bounds = {100, int32_t(100 + (id % 10) * 50), 100, 40};
            if (id % 10 == 0) {
                out->role = TAO_ACCESSIBLE_ROLE_DIALOG; out->child_count = 2;
                out->name = tao::AbiString(dialog == 10 ? "额外设置" : "Mod");
            } else {
                out->role = TAO_ACCESSIBLE_ROLE_BUTTON;
                out->name = tao::AbiString(id % 10 == 1 ? "配置" : "返回");
                out->actions = TAO_ACCESSIBLE_ACTION_FOCUS | TAO_ACCESSIBLE_ACTION_ACTIVATE;
                // 旧弹窗仍存在，甚至仍报告 FOCUS，但已被新的模态挡住。
                if (dialog != s.top) out->flags |= TAO_ACCESSIBLE_BLOCKED;
            }
            return TAO_PLUGIN_OK;
        };
        host.ui.child_at = [](void*, TaoPluginAccessibleNodeV2 n, size_t i, TaoPluginAccessibleNodeV2* out) {
            if (i >= 2) return TAO_PLUGIN_INVALID_ARGUMENT;
            *out = Node(n.widget.object_id + i + 1); return TAO_PLUGIN_OK;
        };
        host.ui.invoke = [](void*, TaoPluginAccessibleNodeV2, uint32_t, double, TaoPluginStringView) {
            return TAO_PLUGIN_OK;
        };
        host.a11y.context = [](void*, TaoPluginAccessibilityContextV1* out) {
            out->flags = TAO_INPUT_MODAL; return TAO_PLUGIN_OK;
        };
        nav.SetSay([this](std::string text, bool) { spoken.push_back(std::move(text)); });
    }
};

int main() {
    int checks = 0;
    const auto require = [&](bool value, const char* message) {
        if (!value) throw std::runtime_error(message); ++checks;
    };
    DialogHost s;
    s.nav.SetDialogAnchor(DialogHost::Node(10));
    s.nav.SyncFocus(DialogHost::Node(11));
    s.top = 20; // 停订期间打开更深一层，故没有 DIALOG_OPEN。
    s.nav.RediscoverDialog();
    require(SameNode(s.nav.Focused(), DialogHost::Node(21)), "new modal replaces blocked old focus");
    require(s.spoken.size() == 2 && s.spoken[0] == "Mod" && s.spoken[1] == "配置", "new dialog title then first control");
    s.nav.Next(1);
    require(SameNode(s.nav.Focused(), DialogHost::Node(22)), "Tab traverses new dialog only");
    s.nav.Next(-1);
    require(SameNode(s.nav.Focused(), DialogHost::Node(21)), "Shift Tab traverses backwards");
    const auto before = s.spoken.size(); const auto calls = s.describes;
    s.nav.RediscoverDialog();
    require(s.spoken.size() == before && s.describes == calls + 1, "same dialog has no repeated speech or subtree scan");
    s.top = 10; s.nav.RediscoverDialog();
    require(SameNode(s.nav.Focused(), DialogHost::Node(11)), "closing inner dialog restores outer navigation without event");
    s.top = 20; s.nav.SyncFocus(DialogHost::Node(22)); s.nav.RediscoverDialog();
    require(SameNode(s.nav.Focused(), DialogHost::Node(22)), "preserve live focus already inside new dialog");
    std::cout << checks << " dialog navigation checks passed\n";
}
