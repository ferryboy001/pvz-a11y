// SPDX-License-Identifier: LGPL-3.0-or-later
// 主菜单绘制文字的值快照；不跨帧保留宿主对象或借用字符串。
#pragma once
#include "HostServices.hpp"
#include <functional>
#include <cmath>
#include <string>
#include <vector>

struct MainMenuButton {
    std::string name;
    TaoPluginRect bounds{};
};

struct MainMenuSnapshot {
    uint64_t selector = 0;
    uint32_t profile = 0;
    std::string name;
    int level = 0;
    bool ready = false;
    std::vector<MainMenuButton> buttons;
};

MainMenuSnapshot A11yReadMainMenu();

class MainMenu {
public:
    using Reader = std::function<MainMenuSnapshot()>;
    explicit MainMenu(Reader reader = A11yReadMainMenu) : reader(std::move(reader)) {}
    void SetSay(std::function<void(std::string, bool)> sink) { say = std::move(sink); }
    std::string LookupName(const TaoPluginAccessibleInfoV2& info) const {
        if (info.scene != 1 || info.dialog.object_id || info.role != TAO_ACCESSIBLE_ROLE_BUTTON) return {};
        const auto current = reader();
        for (const auto& button : current.buttons) {
            const auto& bounds = button.bounds;
            if (std::abs(info.bounds.x - bounds.x) <= 1 && std::abs(info.bounds.y - bounds.y) <= 1 &&
                std::abs(info.bounds.width - bounds.width) <= 1 && std::abs(info.bounds.height - bounds.height) <= 1)
                return button.name;
        }
        return {};
    }

    // 弹窗、用户名面板或加载动画期间等待，不把底层菜单信息插进当前对话。
    void Pump(int scene, uint32_t flags, bool enabled) {
        if (scene != 1 || !enabled) { announced = {}; return; }
        if (flags & TAO_INPUT_MODAL) return;
        const auto current = reader();
        if (!current.ready || current.name.empty()) return;
        if (current.selector == announced.selector && current.profile == announced.profile &&
            current.name == announced.name && current.level == announced.level) return;
        announced = current;
        Say(Summary(current), false);
    }

    bool ReadSummary(int scene, uint32_t flags) const {
        if (scene != 1 || (flags & TAO_INPUT_MODAL)) return false;
        const auto current = reader();
        if (!current.ready) return false;
        Say(Summary(current), true);
        return true;
    }
    bool ReadProgress(int scene, uint32_t flags) const {
        if (scene != 1 || (flags & TAO_INPUT_MODAL)) return false;
        const auto current = reader();
        if (!current.ready) return false;
        Say("冒险模式，" + Progress(current.level), true);
        return true;
    }

    // 保留控件操作名称；只在最终朗读时补充文字，不影响菜单排序和激活门禁。
    std::string Details(const std::string& name, const TaoPluginAccessibleInfoV2& info) const {
        if (info.scene != 1 || info.dialog.object_id || (info.flags & TAO_ACCESSIBLE_BLOCKED) ||
            info.role != TAO_ACCESSIBLE_ROLE_BUTTON) return {};
        if (name != "冒险模式" && name != "用户名牌" && name != "更改用户") return {};
        const auto current = reader();
        if (!current.ready) return {};
        if (name == "冒险模式") return "，" + Progress(current.level);
        if (name == "更改用户") return "，当前存档：" + ProfileName(current) + "。如果这不是你的存档，请点我！";
        return "，欢迎回来，我的朋友！当前存档：" + ProfileName(current);
    }

private:
    static std::string ProfileName(const MainMenuSnapshot& current) {
        return current.name.empty() ? "未选择用户" : current.name;
    }
    static std::string Progress(int level) {
        if (level < 1 || level > 50) return "关卡进度暂不可用";
        return "关卡 " + std::to_string((level - 1) / 10 + 1) + "-" + std::to_string((level - 1) % 10 + 1);
    }
    static std::string Summary(const MainMenuSnapshot& current) {
        return "欢迎回来，我的朋友！当前存档：" + ProfileName(current) +
            "。冒险模式，" + Progress(current.level) + "。如果这不是你的存档，请选择更改用户。";
    }
    void Say(std::string text, bool interrupt) const { if (say) say(std::move(text), interrupt); }
    Reader reader;
    MainMenuSnapshot announced;
    std::function<void(std::string, bool)> say;
};
