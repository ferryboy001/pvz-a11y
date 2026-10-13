// SPDX-License-Identifier: LGPL-3.0-or-later
#include "ChallengeNames.hpp"
#include <iostream>
#include <map>
#include <stdexcept>

// 本测试只模拟同步借用的页字段；实际 SDK 布局由插件的桥接文件编译验证。
int A11yChallengePage(const void* screen) { return screen ? *static_cast<const int*>(screen) : -1; }
std::string __cdecl TodStringTranslate(std::string_view key) { return std::string(key); }

struct FakeInterception {
    TaoPluginInterceptionV1 api{};
    TaoPluginHostV2 host{};
    std::map<std::string, TaoInterceptionReplacementV1> callbacks;
    void* screen = nullptr;
    int page = 0, originals = 0;
    static constexpr const char* points[] = {
        "LawnApp::ShowChallengeScreen", "LawnApp::KillChallengeScreen", "ChallengeScreen::UpdateButtons"
    };
    FakeInterception() {
        api.host_context = this;
        api.describe = [](void*, size_t index, TaoInterceptionPointV1* out) {
            if (index >= std::size(points)) return TAO_PLUGIN_INVALID_ARGUMENT;
            out->id = tao::AbiString(points[index]);
            out->signature = tao::AbiString("fake");
            return TAO_PLUGIN_OK;
        };
        api.install = [](void* data, const TaoInterceptionReplacementV1* spec, TaoPluginToken* token) {
            auto& self = *static_cast<FakeInterception*>(data);
            self.callbacks[std::string(spec->id.data, spec->id.size)] = *spec;
            *token = self.callbacks.size();
            return TAO_PLUGIN_OK;
        };
        api.argument = [](void* data, TaoPluginToken, size_t index, void** out) {
            auto& self = *static_cast<FakeInterception*>(data);
            *out = index == 0 ? static_cast<void*>(&self.screen) : static_cast<void*>(&self.page);
            return TAO_PLUGIN_OK;
        };
        api.original = [](void* data, TaoPluginToken) {
            ++static_cast<FakeInterception*>(data)->originals;
            return TAO_PLUGIN_OK;
        };
        host.host_context = this;
        host.query_interface = [](void* data, TaoPluginStringView, uint32_t, void* out, size_t size) {
            if (size != sizeof(TaoPluginInterceptionV1)) return TAO_PLUGIN_INVALID_ARGUMENT;
            *static_cast<TaoPluginInterceptionV1*>(out) = static_cast<FakeInterception*>(data)->api;
            return TAO_PLUGIN_OK;
        };
    }
    void Invoke(size_t index) {
        auto& spec = callbacks.at(points[index]);
        const int before = originals;
        if (spec.callback(spec.context, 1) != TAO_PLUGIN_OK || originals != before + 1)
            throw std::runtime_error("original must execute exactly once");
    }
};

static void Require(bool condition) { if (!condition) throw std::runtime_error("challenge name regression"); }

int main() {
    try {
        FakeInterception host;
        ChallengeNames names;
        Require(names.Install(&host.host));
        host.Invoke(0); // 从生存页进入，与用户日志一致。
        auto lookup = [&](int row, int col) {
            return names.Lookup(7, TAO_ACCESSIBLE_ROLE_BUTTON,
                {38 + 155 * col, 93 + 119 * row, 104, 115});
        };
        Require(lookup(0, 0).empty());
        host.screen = &host.page;
        host.page = 2;
        host.Invoke(2); // 原地翻页，不调用 ShowChallengeScreen。
        const char* expected[] = {
            "坚果艺术锦标赛", "晴天", "无草皮之地", "重要时刻", "向日葵艺术锦标赛",
            "空袭", "冰冻关卡", "禅境花园", "高重力", "坟墓危机",
            "挖土挑战", "暴风雨夜", "蹦极闪电战", "松鼠", "智慧树",
            "生存模式：白天无尽", "生存模式：黑夜无尽", "生存模式：浓雾无尽", "生存模式：屋顶无尽", "推广页"
        };
        for (int i = 0; i < 20; ++i) Require(lookup(i / 5, i % 5) == expected[i]);
        host.page = 1; host.Invoke(2);
        Require(lookup(0, 0) == "植物僵尸");
        host.page = 3; host.Invoke(2);
        Require(lookup(0, 0) == "破罐者 1");
        host.page = 0; host.Invoke(2);
        Require(names.Lookup(7, TAO_ACCESSIBLE_ROLE_BUTTON, {38, 125, 104, 115}) == "生存模式：白天");
        Require(names.Lookup(1, TAO_ACCESSIBLE_ROLE_BUTTON, {38, 125, 104, 115}).empty());
        Require(names.Lookup(7, TAO_ACCESSIBLE_ROLE_GENERIC, {38, 125, 104, 115}).empty());
        host.Invoke(1);
        Require(names.Lookup(7, TAO_ACCESSIBLE_ROLE_BUTTON, {38, 125, 104, 115}).empty());
        std::cout << "Challenge page switch and all 20 Limbo names passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
