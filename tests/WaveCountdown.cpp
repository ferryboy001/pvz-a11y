// SPDX-License-Identifier: LGPL-3.0-or-later
#define NOMINMAX
#include <windows.h>
#undef GetMessage
#include "BoardControl.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>

extern "C" void* TaoEngineResolveObject(TaoPluginObjectHandle) noexcept { return nullptr; }
extern "C" void* TaoEngineGetApp() noexcept { return nullptr; }
std::string __cdecl TodStringTranslate(std::string_view text) { return std::string(text); }

struct FakeCountdownBoard {
    HostServices host{};
    tao::Objects objects;
    BoardControl control;

    int scene = 3;
    int mode = 0;
    int numWaves = 10;
    int currentWave = 1;
    int zombieCountdown = 1500;
    bool levelComplete = false;
    bool paused = false;
    uint32_t flags = 0;
    std::string spoken;
    std::vector<std::string> playedSounds;
    std::vector<double> playedVolumes;

    static TaoPluginObjectHandle Handle(uint32_t kind, uint64_t id) {
        TaoPluginObjectHandle h{};
        h.struct_size = sizeof(h); h.kind = kind; h.object_id = id;
        h.generation = h.world_generation = 1;
        return h;
    }
    static TaoPluginStatus Number(TaoPluginValue* out, int n) {
        out->kind = TAO_VALUE_INTEGER; out->value.integer_value = n; return TAO_PLUGIN_OK;
    }
    static TaoPluginStatus Boolean(TaoPluginValue* out, bool b) {
        out->kind = TAO_VALUE_BOOLEAN; out->value.boolean_value = b; return TAO_PLUGIN_OK;
    }

    FakeCountdownBoard() : objects(host.objectsApi), control(&host) {
        host.a11y.host_context = host.objectsApi.host_context = host.views.host_context = host.board.host_context = host.sound.host_context = this;
        host.objectsApi.struct_size = sizeof(host.objectsApi);
        host.objectsApi.interface_version = TAO_PLUGIN_OBJECTS_VERSION_1;
        host.a11y.context = [](void* ctx, TaoPluginAccessibilityContextV1* out) {
            auto& s = *static_cast<FakeCountdownBoard*>(ctx);
            out->board = Handle(TAO_OBJECT_BOARD, 1);
            out->columns = 9; out->rows = 5; out->scene = s.scene;
            out->wave = s.currentWave;
            out->flags = s.flags;
            return TAO_PLUGIN_OK;
        };
        host.objectsApi.get_root = [](void*, uint32_t kind, TaoPluginObjectHandle* out) {
            if (kind != TAO_OBJECT_LAWN_APP) return TAO_PLUGIN_NOT_AVAILABLE;
            *out = Handle(kind, 2); return TAO_PLUGIN_OK;
        };
        host.objectsApi.get = [](void* ctx, TaoPluginObjectHandle h, TaoPluginStringView key, TaoPluginValue* out) {
            auto& s = *static_cast<FakeCountdownBoard*>(ctx);
            const auto name = CopyView(key);
            if (h.kind == TAO_OBJECT_LAWN_APP && name == "game_mode") return Number(out, s.mode);
            if (h.kind == TAO_OBJECT_BOARD) {
                if (name == "background") return Number(out, 0);
                if (name == "paused") return Boolean(out, s.paused);
                if (name == "num_waves") return Number(out, s.numWaves);
                if (name == "current_wave") return Number(out, s.currentWave);
                if (name == "zombie_countdown") return Number(out, s.zombieCountdown);
                if (name == "level_complete") return Boolean(out, s.levelComplete);
                return TAO_PLUGIN_NOT_AVAILABLE;
            }
            return TAO_PLUGIN_NOT_AVAILABLE;
        };
        host.objectsApi.call = [](void*, TaoPluginObjectHandle, TaoPluginStringView,
            const TaoPluginValue*, size_t, TaoPluginValue*) {
            return TAO_PLUGIN_NOT_AVAILABLE;
        };
        host.objectsApi.is_valid = [](void*, TaoPluginObjectHandle h, uint8_t* out) {
            *out = (h.object_id != 0); return TAO_PLUGIN_OK;
        };
        host.views.open = [](void*, TaoPluginObjectHandle, TaoPluginStringView, int64_t, TaoPluginViewHandle*) {
            return TAO_PLUGIN_NOT_AVAILABLE;
        };
        host.views.close = [](void*, TaoPluginViewHandle) { return TAO_PLUGIN_OK; };
        host.views.size = [](void*, TaoPluginViewHandle, size_t* out) { *out = 0; return TAO_PLUGIN_OK; };
        host.views.get = [](void*, TaoPluginViewHandle, size_t, TaoPluginValue*) { return TAO_PLUGIN_NOT_AVAILABLE; };
        host.sound.play = [](void* ctx, const TaoPluginAudioPlayDescriptorV1* desc, TaoPluginAudioHandle* out) {
            auto& s = *static_cast<FakeCountdownBoard*>(ctx);
            s.playedSounds.push_back(std::string(desc->resource_id.data, desc->resource_id.size));
            s.playedVolumes.push_back(desc->volume);
            out->audio_id = s.playedSounds.size();
            return TAO_PLUGIN_OK;
        };
        control.SetSay([this](std::string text, bool) { spoken = std::move(text); });
        control.SetObjects(&objects);
    }

    void Update(uint64_t tick, bool waveCountdownOn = true, int waveCountdownSeconds = 5) {
        const auto ctx = control.Context();
        control.UpdateCues(ctx, tick, false, false, false, waveCountdownOn, waveCountdownSeconds);
    }
};

int main() {
    int checks = 0;
    const auto require = [&](bool ok, const char* why) {
        if (!ok) throw std::runtime_error(why);
        ++checks;
    };

    try {
        // 1. 测试 AnnounceWave 读取下一波刷新时间
        {
            FakeCountdownBoard s;
            s.zombieCountdown = 1250;
            s.control.AnnounceWave();
            require(s.spoken == "进度：第 1 波，下一波约 13 秒", "AnnounceWave includes rounded remaining seconds");
            s.zombieCountdown = 0;
            s.control.AnnounceWave();
            require(s.spoken == "进度：第 1 波", "AnnounceWave without countdown when countdown is 0");
        }

        // 2. 测试 5 秒倒计时正常逐秒递降（G F E D C -> 5, 4, 3, 2, 1）
        {
            FakeCountdownBoard s;
            uint64_t tick = 100;
            s.zombieCountdown = 600;
            s.Update(tick++, true, 5);
            require(s.playedSounds.empty(), "no sound above 500cs");

            // 到达 500cs（5秒）
            s.zombieCountdown = 500;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 1 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_5",
                    "5s left plays SOUND_PVZ_A11Y_COUNTDOWN_5 (G5)");

            // 490cs 不重复播放
            s.zombieCountdown = 490;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 1, "no duplicate play in same second");

            // 到达 400cs（4秒）
            s.zombieCountdown = 400;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 2 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_4",
                    "4s left plays SOUND_PVZ_A11Y_COUNTDOWN_4 (F5)");

            // 到达 300cs（3秒）
            s.zombieCountdown = 300;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 3 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_3",
                    "3s left plays SOUND_PVZ_A11Y_COUNTDOWN_3 (E5)");

            // 到达 200cs（2秒）
            s.zombieCountdown = 200;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 4 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_2",
                    "2s left plays SOUND_PVZ_A11Y_COUNTDOWN_2 (D5)");

            // 到达 100cs（1秒）
            s.zombieCountdown = 100;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 5 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_1",
                    "1s left plays SOUND_PVZ_A11Y_COUNTDOWN_1 (C5)");

            // 到达 0cs（波次刷新到场）
            s.zombieCountdown = 0;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 5, "no additional countdown sound at 0cs");
        }

        // 3. 测试设定的提醒时间越长，起始音高相应提高（7秒提醒倒计时：起始音为 7 = B5）
        {
            FakeCountdownBoard s;
            uint64_t tick = 100;
            s.zombieCountdown = 700;
            s.Update(tick++, true, 7);
            require(s.playedSounds.size() == 1 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_7",
                    "7s countdown starts at COUNTDOWN_7 (B5)");
        }

        // 4. 测试刷新时间突降时及时反应（例如击杀加快，倒计时突降到 200cs）
        {
            FakeCountdownBoard s;
            uint64_t tick = 100;
            s.zombieCountdown = 1500; // 仍在常规阶段
            s.Update(tick++, true, 5);
            require(s.playedSounds.empty(), "quiet before countdown threshold");

            // 突然掉到 200cs（2秒）
            s.zombieCountdown = 200;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 1 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_2",
                    "sudden drop to 200cs immediately triggers COUNTDOWN_2 (D5)");

            // 随后正常到达 100cs（1秒）
            s.zombieCountdown = 100;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 2 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_1",
                    "then triggers COUNTDOWN_1 (C5) at 100cs");
        }

        // 5. 测试倒计时突增重置（例如新波次开始重设为 2500cs）
        {
            FakeCountdownBoard s;
            uint64_t tick = 100;
            s.zombieCountdown = 200;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 1 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_2",
                    "plays COUNTDOWN_2");

            // 新波次重置
            s.zombieCountdown = 2500;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 1, "quiet on new wave reset");

            // 新波次倒计时再次降到 500cs
            s.zombieCountdown = 500;
            s.Update(tick++, true, 5);
            require(s.playedSounds.size() == 2 && s.playedSounds.back() == "SOUND_PVZ_A11Y_COUNTDOWN_5",
                    "new wave cleanly counts down starting at COUNTDOWN_5");
        }

        // 6. 测试禁用开关与通关状态
        {
            FakeCountdownBoard s;
            s.zombieCountdown = 300;
            s.Update(100, false, 5);
            require(s.playedSounds.empty(), "disabled wave countdown produces no sound");

            s.levelComplete = true;
            s.Update(101, true, 5);
            require(s.playedSounds.empty(), "level complete produces no sound");
        }

        {
            FakeCountdownBoard s; s.control.SetCueVolumes(80, 35, 60);
            s.zombieCountdown = 300; s.Update(100);
            require(s.playedVolumes.size() == 1 && s.playedVolumes[0] == .35, "gameplay countdown uses independent volume");
            s.control.PreviewCountdownCue(); require(s.playedVolumes.back() == .35, "preview matches gameplay countdown gain");
            s.control.SetCueVolumes(80, 0, 60); s.zombieCountdown = 200;
            const auto count = s.playedSounds.size(); s.Update(101);
            require(s.playedSounds.size() == count, "zero countdown volume mutes gameplay");
        }
        std::cout << checks << " wave countdown checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
