// SPDX-License-Identifier: LGPL-3.0-or-later
#include <Lawn/Widget/ChallengeScreen.h>

// 使用 SDK 的真实类型布局，避免硬编码偏移或借用原生对象到调用期之外。
int A11yChallengePage(const void* screen) {
    return screen ? int(static_cast<const ChallengeScreen*>(screen)->mPageIndex) : -1;
}
