#include "UiNavigation.hpp"
#include "MainMenu.hpp"
#include <iostream>
#include <map>
#include <stdexcept>

struct FakeUi {
    struct Entry {
        TaoPluginAccessibleInfoV2 info{};
        TaoPluginObjectHandle parent{};
        std::string name;
        std::string type = "ButtonWidget";
        std::vector<uint64_t> children;
        bool rejectFocus = false;
    };
    std::map<uint64_t, Entry> entries;
    HostServices services{};
    int scene = 1;
    uint32_t flags = 0;
    uint32_t lastAction = 0;

    TaoPluginAccessibleNodeV2 Node(uint64_t identity) {
        TaoPluginAccessibleNodeV2 node{};
        node.widget.object_id = identity;
        node.widget.generation = 1;
        node.widget.world_generation = 1;
        return node;
    }
    void Add(uint64_t identity, std::string name, TaoPluginRect bounds, uint64_t parent = 1) {
        auto& entry = entries[identity];
        entry.name = std::move(name);
        entry.info.node = Node(identity);
        entry.info.role = TAO_ACCESSIBLE_ROLE_BUTTON;
        entry.info.flags = TAO_WIDGET_VISIBLE;
        entry.info.actions = uint32_t(TAO_ACCESSIBLE_ACTION_FOCUS) | TAO_ACCESSIBLE_ACTION_ACTIVATE;
        entry.info.bounds = bounds;
        entry.parent = Node(parent).widget;
        entries[parent].children.push_back(identity);
    }
    FakeUi() {
        entries[1].type = "GameSelector";
        services.ui.host_context = this;
        services.widgets.host_context = this;
        services.a11y.host_context = this;
        services.widgets.root_count = [](void*, size_t* count) { *count = 1; return TAO_PLUGIN_OK; };
        services.widgets.root_at = [](void* opaque, size_t, TaoPluginObjectHandle* out) {
            *out = static_cast<FakeUi*>(opaque)->Node(1).widget;
            return TAO_PLUGIN_OK;
        };
        services.widgets.describe = [](void* opaque, TaoPluginObjectHandle handle, TaoPluginWidgetInfoV1* out) {
            auto& state = *static_cast<FakeUi*>(opaque);
            const auto found = state.entries.find(handle.object_id);
            if (found == state.entries.end()) return TAO_PLUGIN_STALE_HANDLE;
            out->parent = found->second.parent;
            out->type_name = tao::AbiString(found->second.type);
            return TAO_PLUGIN_OK;
        };
        services.a11y.context = [](void* opaque, TaoPluginAccessibilityContextV1* out) {
            auto& state = *static_cast<FakeUi*>(opaque);
            out->scene = state.scene;
            out->flags = state.flags;
            return TAO_PLUGIN_OK;
        };
        services.ui.describe = [](void* opaque, TaoPluginAccessibleNodeV2 node, TaoPluginAccessibleInfoV2* out) {
            auto& state = *static_cast<FakeUi*>(opaque);
            const auto found = state.entries.find(node.widget.object_id);
            if (found == state.entries.end()) return TAO_PLUGIN_STALE_HANDLE;
            *out = found->second.info;
            out->scene = state.scene;
            out->name = tao::AbiString(found->second.name);
            out->child_count = found->second.children.size();
            return TAO_PLUGIN_OK;
        };
        services.ui.child_at = [](void* opaque, TaoPluginAccessibleNodeV2 node, size_t index,
            TaoPluginAccessibleNodeV2* out) {
            auto& state = *static_cast<FakeUi*>(opaque);
            *out = state.Node(state.entries.at(node.widget.object_id).children.at(index));
            return TAO_PLUGIN_OK;
        };
        services.ui.invoke = [](void* opaque, TaoPluginAccessibleNodeV2 node, uint32_t action,
            double, TaoPluginStringView) {
            auto& state = *static_cast<FakeUi*>(opaque);
            state.lastAction = action;
            if (action == TAO_ACCESSIBLE_ACTION_FOCUS) {
                auto& target = state.entries.at(node.widget.object_id);
                if (target.rejectFocus) return TAO_PLUGIN_NOT_AVAILABLE;
                for (auto& entry : state.entries) entry.second.info.flags &= ~TAO_WIDGET_FOCUSED;
                target.info.flags |= TAO_WIDGET_FOCUSED;
            }
            return TAO_PLUGIN_OK;
        };
        Add(90, "退出", {715, 510, 50, 50});
        Add(23, "", {406, 173, 300, 80});
        Add(70, "", {405, 79, 300, 100});
        Add(44, "", {410, 257, 300, 80});
        Add(12, "", {413, 328, 300, 80});
        Add(31, "", {405, 482, 130, 89});
    }
};

MainMenuSnapshot A11yReadMainMenu() { return {}; }

int main() {
    FakeUi state;
    UiNavigation navigation(&state.services);
    int checks = 0;
    const auto require = [&](bool ok, const char* label) {
        if (!ok) throw std::runtime_error(label);
        ++checks;
    };
    MainMenuSnapshot snapshot{1, 7, "123", 1, true};
    MainMenu menu([&] { return snapshot; });
    std::vector<std::string> speeches;
    menu.SetSay([&](std::string text, bool) { speeches.push_back(std::move(text)); });
    menu.Pump(1, 0, true);
    require(speeches.size() == 1 && speeches.back().find("当前存档：123") != std::string::npos &&
        speeches.back().find("关卡 1-1") != std::string::npos, "entry must read actual profile and level");
    menu.Pump(1, 0, true);
    require(speeches.size() == 1, "unchanged menu must remain silent");
    snapshot.profile = 8; snapshot.name = "新用户"; snapshot.level = 11;
    menu.Pump(1, TAO_INPUT_MODAL, true);
    require(speeches.size() == 1 && !menu.ReadSummary(1, TAO_INPUT_MODAL), "dialog must suppress underlying menu");
    snapshot.ready = false; menu.Pump(1, 0, true);
    require(speeches.size() == 1 && !menu.ReadProgress(1, 0), "user panel/loading must wait");
    snapshot.ready = true; menu.Pump(1, 0, true);
    require(speeches.size() == 2 && speeches.back().find("新用户") != std::string::npos &&
        speeches.back().find("关卡 2-1") != std::string::npos, "profile switch must refresh once after dialog closes");
    snapshot.name = "重命名用户"; menu.Pump(1, 0, true);
    require(speeches.back().find("重命名用户") != std::string::npos, "rename on same profile must refresh");
    snapshot.level = 10; menu.ReadProgress(1, 0);
    require(speeches.back() == "冒险模式，关卡 1-10", "chapter boundary 10");
    snapshot.level = 50; menu.ReadProgress(1, 0);
    require(speeches.back() == "冒险模式，关卡 5-10", "last adventure level");
    snapshot.level = 0; menu.ReadProgress(1, 0);
    require(speeches.back().find("暂不可用") != std::string::npos, "unknown level must not invent 1-1");
    require(!menu.ReadSummary(3, 0), "board must retain its existing F2 behavior");
    snapshot.level = 1; menu.Pump(3, 0, true);
    const auto count = speeches.size(); menu.Pump(1, 0, true);
    require(speeches.size() == count + 1, "returning from gameplay must announce again");
    menu.Pump(1, 0, false); menu.Pump(1, 0, true);
    require(speeches.size() == count + 2, "re-enabling accessibility must announce");
    snapshot.buttons = {{"更改用户", {49,136,250,30}}, {"僵尸大头贴", {32,180,279,83}},
        {"联机模式", {265,364,86,54}}, {"冒险模式", {405,79,300,100}}};
    navigation.SetMainMenuName([&](const auto& info) { return menu.LookupName(info); });
    navigation.SetSpeechDetails([&](const std::string& name, const auto& info) { return menu.Details(name, info); });
    navigation.SetSay([&](std::string text, bool) { speeches.push_back(std::move(text)); });
    state.Add(40, "", {49,136,250,30});
    state.Add(41, "", {32,180,279,83});
    state.Add(42, "", {265,364,86,54});
    TaoPluginAccessibleInfoV2 menuInfo{};
    state.services.ui.describe(&state, state.Node(40), &menuInfo);
    require(navigation.Name(state.Node(40), menuInfo) == "更改用户", "name plate is change user");
    state.services.ui.describe(&state, state.Node(41), &menuInfo);
    require(navigation.Name(state.Node(41), menuInfo) == "僵尸大头贴", "left billboard is zombatar");
    state.services.ui.describe(&state, state.Node(42), &menuInfo);
    require(navigation.Name(state.Node(42), menuInfo) == "联机模式", "small tombstone is multiplayer");
    navigation.Focus(state.Node(70));
    require(speeches.back() == "冒险模式，关卡 1-1", "focus must read progress");
    state.services.ui.describe(&state, state.Node(70), &menuInfo);
    require(navigation.Name(state.Node(70), menuInfo) == "冒险模式", "speech detail must not change operation name");
    navigation.Focus(state.Node(40));
    require(speeches.back().find("当前存档：重命名用户") != std::string::npos, "change user focus must read profile");
    navigation.SetBeforeActivate([&](const auto&, const auto&, const std::string& name) {
        require(name == "更改用户", "activation must receive undecorated name"); return true;
    });
    require(navigation.ActivateFocused(), "activation remains handled");
    navigation.SetBeforeActivate({});
    // 实际宿主把所有类型报成 Widget，仍须按原生按钮身份维持顺序。
    state.entries[1].type = "Widget";
    navigation.SyncFocus(state.Node(40)); navigation.Next(1);
    require(navigation.Focused().widget.object_id == 42, "flat host must navigate from change user to multiplayer");
    state.entries[1].type = "GameSelector";
    // 恢复原有导航测试的菜单集合。
    state.entries[1].children.erase(std::remove_if(state.entries[1].children.begin(), state.entries[1].children.end(),
        [](uint64_t id) { return id == 40 || id == 41 || id == 42; }), state.entries[1].children.end());
    navigation.SyncFocus({});
    const auto expect = [&](uint64_t identity, const char* label) {
        if (navigation.Focused().widget.object_id != identity) throw std::runtime_error(label);
        ++checks;
    };
    navigation.Arrow(0, 1);
    expect(70, "initial focus must be adventure");
    navigation.Arrow(1, 0);
    expect(23, "right must follow menu order");
    navigation.Arrow(0, 1);
    expect(44, "down must follow menu order");
    std::reverse(state.entries[1].children.begin(), state.entries[1].children.end());
    navigation.Next(1);
    expect(12, "enumeration order must not affect Tab");
    navigation.Arrow(-1, 0);
    expect(44, "left must reverse menu order");
    navigation.Arrow(0, -1);
    expect(23, "up must reverse menu order");
    state.entries[44].info.flags |= TAO_WIDGET_DISABLED;
    navigation.Next(1);
    expect(12, "disabled controls must be skipped");
    state.entries[31].info.flags &= ~TAO_WIDGET_VISIBLE;
    navigation.Next(1);
    expect(90, "hidden controls must be skipped");
    navigation.Next(1);
    expect(70, "forward wrapping");
    navigation.Next(-1);
    expect(90, "reverse wrapping");
    state.entries[31].info.flags |= TAO_WIDGET_VISIBLE;
    navigation.Next(-1);
    expect(31, "store must have a fixed name and position in order");
    state.entries[23].info.bounds = {103, 342, 200, 50};
    navigation.SyncFocus(state.Node(70));
    navigation.Arrow(1, 0);
    expect(23, "animation must preserve identity and order");
    TaoPluginAccessibleInfoV2 info{};
    state.services.ui.describe(&state, state.Node(23), &info);
    if (navigation.Name(state.Node(23), info) != "迷你游戏") throw std::runtime_error("cached menu name");
    ++checks;
    state.entries[23].rejectFocus = true;
    navigation.SyncFocus(state.Node(70));
    navigation.Next(1);
    expect(12, "rejected focus must try the next available item");
    state.entries[23].rejectFocus = false;
    navigation.SyncFocus(state.Node(999));
    navigation.Arrow(0, 1);
    expect(70, "stale focus must recover at the first menu item");
    state.Add(150, "是", {200, 250, 100, 30}, 200);
    state.Add(151, "否", {400, 250, 100, 30}, 200);
    state.entries[200].type = "Dialog";
    state.entries[150].info.dialog = state.Node(200).widget;
    state.entries[151].info.dialog = state.Node(200).widget;
    state.flags = TAO_INPUT_MODAL;
    navigation.SetDialogAnchor(state.Node(200));
    navigation.SyncFocus(state.Node(150));
    navigation.Arrow(1, 0);
    expect(151, "dialog navigation must remain spatial and isolated");
    navigation.Arrow(0, 1);
    expect(151, "dialog edge must not wrap");
    state.entries[151].info.role = TAO_ACCESSIBLE_ROLE_SLIDER;
    navigation.Arrow(-1, 0);
    if (state.lastAction != TAO_ACCESSIBLE_ACTION_FOCUS) throw std::runtime_error("slider refocus");
    expect(151, "slider adjustment must keep focus");
    std::cout << checks << " main menu reading and navigation checks passed\n";
}
