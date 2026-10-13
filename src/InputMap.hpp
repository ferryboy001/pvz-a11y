// SPDX-License-Identifier: LGPL-3.0-or-later
// 可重绑定输入映射。动作与默认键位对齐 PvZA11y（见其 readme.txt），
// 后续从玩家档案加载用户绑定并开放重绑定界面。
#pragma once
#include <vector>

enum class Action {
    // PvZA11y 常规操作
    Up, Down, Left, Right, Confirm, Deny, Start, Option,
    CycleLeft, CycleRight, Info1, Info2, Info3, Info4,
    Slot, SlotPull, FireCannon, ZombieMinus, ZombiePlus, ZombieFilter, Shovel, RowInfo,
    // 框架自身
    ToggleMod, Help, StepFrame, Store, Settings,
    DumpTree, // 临时调试
};

struct Binding {
    int code;
    Action action;
    int param = 0;
    bool repeat = false;
    int device = 0; // 0 = 键盘，1 = 手柄
};

class InputMap {
public:
    bool Find(int device, int code, bool repeat, Action& action, int& param) const {
        for (const auto& binding : bindings)
            if (binding.device == device && binding.code == code && (!repeat || binding.repeat)) {
                action = binding.action;
                param = binding.param;
                return true;
            }
        return false;
    }

private:
    static std::vector<Binding> DefaultBindings() {
        std::vector<Binding> result = {
            // 键盘（PvZA11y 默认）
            {38, Action::Up, 0, true}, {40, Action::Down, 0, true},
            {37, Action::Left, 0, true}, {39, Action::Right, 0, true},
            {13, Action::Confirm}, {8, Action::Deny}, {27, Action::Start}, {9, Action::Option},
            {189, Action::CycleLeft}, {187, Action::CycleRight},
            {112, Action::Info1}, {113, Action::Info2}, {114, Action::Info3}, {115, Action::Info4},
            {192, Action::Shovel}, {146, Action::Shovel}, // 192=VK_OEM_3 标准布局；146=部分布局/设备上报的反引号（实测）
            // 注意：宿主键码表对 OEM 标点统一上报 0（实测），188/190/189/187 永远不会
            // 命中；逗号/句号/减号/等号由 Plugin.cpp 的 PollOemPunctuation 按 VK 甄别。
            {188, Action::ZombieMinus}, {190, Action::ZombiePlus}, {117, Action::ZombieFilter},
            {16, Action::RowInfo}, {160, Action::RowInfo}, {161, Action::RowInfo}, // Shift（含左右 Shift 原生键码）
            // 键盘（框架自身）
            {119, Action::ToggleMod}, {121, Action::Help}, {'N', Action::StepFrame},
            {118, Action::Store}, // F7 打开/重读疯狂戴夫的商店
            {122, Action::Settings}, // F11 打开无障碍 Mod 设置
            {123, Action::DumpTree}, // 临时调试（F12）
            {'S', Action::SlotPull}, // 老虎机拉杆
            {'C', Action::FireCannon}, // 向当前格快捷炮击
            // 手柄（PvZA11y 默认，按钮编号沿用宿主标准映射）
            {11, Action::Up, 0, true, 1}, {12, Action::Down, 0, true, 1},
            {13, Action::Left, 0, true, 1}, {14, Action::Right, 0, true, 1},
            {0, Action::Confirm, 0, false, 1}, {1, Action::Deny, 0, false, 1},
            {6, Action::Start, 0, false, 1}, {4, Action::Option, 0, false, 1},
            {9, Action::CycleLeft, 0, false, 1}, {10, Action::CycleRight, 0, false, 1},
            {2, Action::Info1, 0, false, 1}, {3, Action::Info2, 0, false, 1},
            {7, Action::Info3, 0, false, 1}, {8, Action::Info4, 0, false, 1},
        };
        for (int i = 0; i < 9; ++i) result.push_back({'1' + i, Action::Slot, i});
        result.push_back({'0', Action::Slot, 9});
        return result;
    }

    std::vector<Binding> bindings = DefaultBindings();
};
