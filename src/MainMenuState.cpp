// SPDX-License-Identifier: LGPL-3.0-or-later
#include "MainMenu.hpp"
#include <LawnApp.h>
#include <Lawn/Widget/GameSelector.h>
#include <Lawn/System/PlayerInfo.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" LawnApp* TaoEngineGetApp() noexcept;

MainMenuSnapshot A11yReadMainMenu() {
#if defined(_WIN32)
    // 与现有原生桥相同：仅链接导入库，不引入 Access.hpp 的编译器版本守卫。
    static const bool available = GetProcAddress(GetModuleHandleW(nullptr), "TaoEngineGetApp") != nullptr;
    if (!available) return {};
#endif
    const auto* app = TaoEngineGetApp();
    if (!app || app->mGameScene != SCENE_MENU || !app->mGameSelector) return {};
    const auto* selector = app->mGameSelector;
    MainMenuSnapshot result;
    result.selector = reinterpret_cast<uintptr_t>(selector);
    result.level = selector->mLevel; // 与冒险按钮绘制的 Level X-Y 共用读数。
    result.ready = !selector->mLoading && !selector->mStartingGame &&
        selector->mSelectorState == SELECTOR_IDLE && selector->mX == 0 && selector->mY == 0;
    if (const auto* profile = app->mPlayerInfo) {
        result.profile = profile->mId;
        result.name = profile->mName;
    }
    const auto add = [&](const Sexy::Widget* button, const char* name) {
        if (!button) return;
        int x = button->mX, y = button->mY;
        const auto* parent = button->mParent;
        for (int depth = 0; parent && depth < 16; ++depth, parent = parent->mParent) {
            x += parent->mX; y += parent->mY;
        }
        result.buttons.push_back({name, {x, y, button->mWidth, button->mHeight}});
    };
    add(selector->mAdventureButton, "冒险模式");
    add(selector->mMinigameButton, "迷你游戏");
    add(selector->mPuzzleButton, "解谜模式");
    add(selector->mSurvivalButton, "生存模式");
    add(selector->mZenGardenButton, "禅境花园");
    add(selector->mStoreButton, "商店");
    add(selector->mAlmanacButton, "图鉴");
    add(selector->mAchievementsButton, "成就");
    add(selector->mZombatarButton, "僵尸大头贴");
    add(selector->mChangeUserButton, "更改用户");
    add(selector->mQuickPlayButton, "快速游戏");
    add(selector->mMultiplayerButton, "联机模式");
    add(selector->mOptionsButton, "选项");
    add(selector->mHelpButton, "帮助");
    add(selector->mQuitButton, "退出");
    return result;
}
