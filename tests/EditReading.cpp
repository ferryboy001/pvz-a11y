// SPDX-License-Identifier: LGPL-3.0-or-later
#include "UiNavigation.hpp"
#include <iostream>
#include <stdexcept>

struct FakeUi {
    HostServices host{};
    UiNavigation nav{&host};
    std::string text = "125", caption = "阳光", borrowedName, borrowedText;
    double caret = 3;
    bool alive = true, focused = true, editing = true;
    int describes = 0;
    TaoPluginAccessibleNodeV2 current = Node(2);
    std::vector<std::string> speeches;
    static TaoPluginAccessibleNodeV2 Node(uint64_t id) {
        TaoPluginAccessibleNodeV2 node{};
        node.widget.struct_size = sizeof(node.widget); node.widget.object_id = id;
        node.widget.generation = node.widget.world_generation = 1;
        return node;
    }
    FakeUi() {
        host.ui.host_context = host.widgets.host_context = host.a11y.host_context = this;
        host.a11y.context = [](void* ctx, TaoPluginAccessibilityContextV1* out) {
            auto& s = *static_cast<FakeUi*>(ctx); out->focus = s.current;
            out->flags = s.editing ? TAO_INPUT_TEXT_EDIT : 0; return TAO_PLUGIN_OK;
        };
        host.widgets.describe = [](void*, TaoPluginObjectHandle h, TaoPluginWidgetInfoV1* out) {
            out->type_name = tao::AbiString(h.object_id == 2 ? "ModUiEditWidget" : "ModUiNativeWidget");
            out->parent = h.object_id == 1 ? TaoPluginObjectHandle{} : Node(1).widget;
            return TAO_PLUGIN_OK;
        };
        host.ui.describe = [](void* ctx, TaoPluginAccessibleNodeV2 node, TaoPluginAccessibleInfoV2* out) {
            auto& s = *static_cast<FakeUi*>(ctx);
            ++s.describes;
            if (!s.alive && node.widget.object_id == 2) return TAO_PLUGIN_STALE_HANDLE;
            // 模拟宿主 thread_local 借用字符串：每次 describe 都会覆盖前一次的视图。
            s.borrowedName.clear(); s.borrowedText.clear();
            out->flags = TAO_WIDGET_VISIBLE;
            if (node.widget.object_id == 1) { out->role = TAO_ACCESSIBLE_ROLE_GENERIC; out->child_count = 2; }
            if (node.widget.object_id == 2) {
                out->role = TAO_ACCESSIBLE_ROLE_EDIT;
                out->actions = TAO_ACCESSIBLE_ACTION_FOCUS | TAO_ACCESSIBLE_ACTION_SET_TEXT;
                if (s.focused) out->flags |= TAO_WIDGET_FOCUSED;
                s.borrowedName = s.text; s.borrowedText = s.text;
                out->value = s.caret; out->bounds = {152,96,110,30};
            }
            if (node.widget.object_id == 3) {
                out->role = TAO_ACCESSIBLE_ROLE_GENERIC;
                s.borrowedName = s.caption; s.borrowedText = s.caption;
                out->bounds = {20,96,130,28};
            }
            out->name = tao::AbiString(s.borrowedName); out->text = tao::AbiString(s.borrowedText);
            return TAO_PLUGIN_OK;
        };
        host.ui.child_at = [](void*, TaoPluginAccessibleNodeV2, size_t i, TaoPluginAccessibleNodeV2* out) {
            if (i >= 2) return TAO_PLUGIN_INVALID_ARGUMENT;
            *out = Node(i == 0 ? 3 : 2); return TAO_PLUGIN_OK;
        };
        host.ui.invoke = [](void*, TaoPluginAccessibleNodeV2, uint32_t, double, TaoPluginStringView) { return TAO_PLUGIN_OK; };
        nav.SetSay([this](std::string text, bool) { speeches.push_back(std::move(text)); });
    }
};

int main() {
    int checks = 0;
    const auto require = [&](bool ok, const char* why) { if (!ok) throw std::runtime_error(why); ++checks; };
    FakeUi s;
    s.editing = false; s.nav.PumpEdit();
    require(s.describes == 0 && s.speeches.empty(), "non-edit board focus must not be described");
    s.editing = true;
    s.nav.PumpEdit();
    require(s.speeches.back() == "阳光，编辑框，当前值：125，插入点在末尾", "mouse/native focus reads label and current value");
    s.nav.PumpEdit(); require(s.speeches.size() == 1, "unchanged frame must be silent");
    s.caret = 2; s.nav.PumpEdit();
    require(s.speeches.back() == "5，插入点在第2个字符后", "left reads crossed character");
    s.caret = 0; s.nav.PumpEdit(); require(s.speeches.back() == "12，插入点在开头", "Home movement");
    s.text = "9125"; s.caret = 1; s.nav.PumpEdit();
    require(s.speeches.back() == "输入 9，插入点在第1个字符后", "insertion echo");
    s.text = "125"; s.caret = 0; s.nav.PumpEdit();
    require(s.speeches.back() == "删除 9，插入点在开头", "Backspace/Delete echo");
    s.nav.ReadCurrentEdit();
    require(s.speeches.back() == "阳光，编辑框，当前值：125，插入点在开头", "F1 reads full current value without refocusing");
    s.text.clear(); s.caret = 0; s.nav.PumpEdit();
    require(s.speeches.back() == "删除 125，插入点在开头", "clear all echo");
    s.nav.ReadCurrentEdit(); require(s.speeches.back().find("当前值：空") != std::string::npos, "empty value");
    s.nav.SyncFocus({}); s.text = "你好A"; s.caret = 7;
    require(s.nav.Focus(FakeUi::Node(2)), "semantic focus on edit");
    const size_t count = s.speeches.size(); s.nav.PumpEdit();
    require(s.speeches.size() == count, "semantic focus and host polling must not duplicate entry");
    s.caret = 3; s.nav.PumpEdit();
    require(s.speeches.back() == "好A，插入点在第1个字符后", "UTF-8 byte cursor converts to character position");
    s.text = "你妹A"; s.caret = 6; s.nav.PumpEdit();
    require(s.speeches.back() == "删除 好，输入 妹，插入点在第2个字符后", "replacement with common partial UTF-8 bytes stays valid");
    s.focused = false; s.nav.PumpEdit(); require(s.speeches.size() == count + 2, "blur stays silent");
    s.focused = true; s.nav.PumpEdit(); require(s.speeches.size() == count + 3, "returning focus reads entry again");
    s.alive = false; s.nav.PumpEdit(); require(s.speeches.size() == count + 3, "destroyed field stays silent");
    s.alive = true; s.current.widget.generation = 2; s.nav.PumpEdit();
    require(s.speeches.size() == count + 4, "reused object id with new generation reads fresh entry");
    s.nav.SyncFocus({}); s.caption = "Toolkit 阵型代码";
    s.host.ui.describe = [](void* ctx, TaoPluginAccessibleNodeV2 node, TaoPluginAccessibleInfoV2* out) {
        auto& s = *static_cast<FakeUi*>(ctx); out->flags = TAO_WIDGET_VISIBLE;
        if (node.widget.object_id == 1) { out->role = TAO_ACCESSIBLE_ROLE_GENERIC; out->child_count = 2; }
        if (node.widget.object_id == 2) {
            out->role = TAO_ACCESSIBLE_ROLE_EDIT; out->flags |= TAO_WIDGET_FOCUSED;
            out->text = tao::AbiString(s.text); out->bounds = {20,232,640,36}; out->value = s.caret;
        }
        if (node.widget.object_id == 3) {
            out->role = TAO_ACCESSIBLE_ROLE_GENERIC; out->text = tao::AbiString(s.caption);
            out->bounds = {20,196,640,28};
        }
        return TAO_PLUGIN_OK;
    };
    s.nav.PumpEdit(); require(s.speeches.back().starts_with("Toolkit 阵型代码，编辑框"), "above-field label association");
    std::cout << checks << " edit reading checks passed\n";
}
