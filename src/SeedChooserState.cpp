// SPDX-License-Identifier: LGPL-3.0-or-later
#include <LawnApp.h>
#include <Lawn/Widget/SeedChooserScreen.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" LawnApp* TaoEngineGetApp() noexcept;

bool A11ySeedChooserAnimating() {
#if defined(_WIN32)
    static const bool available = GetProcAddress(GetModuleHandleW(nullptr), "TaoEngineGetApp") != nullptr;
    if (!available) return false;
#endif
    const auto* app = TaoEngineGetApp();
    return app && app->mGameScene == SCENE_LEVEL_INTRO && app->mSeedChooserScreen &&
        app->mSeedChooserScreen->mSeedsInFlight > 0;
}

bool A11ySeedChooserAvailable() {
#if defined(_WIN32)
    static const bool available = GetProcAddress(GetModuleHandleW(nullptr), "TaoEngineGetApp") != nullptr;
    if (!available) return false;
#endif
    const auto* app = TaoEngineGetApp();
    return app && app->mGameScene == SCENE_LEVEL_INTRO && app->mSeedChooserScreen &&
        app->mSeedChooserScreen->mChooseState != CHOOSE_VIEW_LAWN &&
        app->mCrazyDaveState == CRAZY_DAVE_OFF && app->mSeedChooserScreen->mVisible &&
        !app->mSeedChooserScreen->mDisabled && app->mSeedChooserScreen->mY == 0;
}
