// SPDX-License-Identifier: LGPL-3.0-or-later
#define NOMINMAX
#include <windows.h>
#undef GetMessage
#include "BoardControl.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

extern "C" void* TaoEngineResolveObject(TaoPluginObjectHandle) noexcept { return nullptr; }
extern "C" void* TaoEngineGetApp() noexcept { return nullptr; }
std::string __cdecl TodStringTranslate(std::string_view text) { return std::string(text); }

struct FakeCueBoard {
    struct Zombie {
        int type = 23, phase = 0, row = 2, age = 100, x = 400;
        float posX = 400;
        uint64_t generation = 1;
        bool dead = false, phaseReadable = true, posReadable = true, xReadable = true;
    };
    struct Sound { std::string id; int pan; double volume; };
    HostServices host{};
    tao::Objects objects;
    BoardControl control;
    std::vector<Zombie> zombies;
    std::vector<Sound> sounds;
    std::vector<uint64_t> destroyed;
    bool playing = true;
    int background = 0, level = 20, mode = 0, scene = 3;
    uint32_t flags = 0;
    std::vector<std::string> spoken;
    uint64_t boardGeneration = 1, tick = 5;

    static TaoPluginObjectHandle Handle(uint32_t kind, uint64_t id, uint64_t generation = 1) {
        TaoPluginObjectHandle h{};
        h.struct_size = sizeof(h); h.kind = kind; h.object_id = id;
        h.generation = generation; h.world_generation = 1;
        return h;
    }
    static TaoPluginStatus Number(TaoPluginValue* out, int value) {
        out->kind = TAO_VALUE_INTEGER; out->value.integer_value = value; return TAO_PLUGIN_OK;
    }
    static TaoPluginStatus Boolean(TaoPluginValue* out, bool value) {
        out->kind = TAO_VALUE_BOOLEAN; out->value.boolean_value = value; return TAO_PLUGIN_OK;
    }
    FakeCueBoard() : objects(host.objectsApi), control(&host) {
        host.a11y.host_context = host.objectsApi.host_context = host.views.host_context = host.sound.host_context = this;
        host.objectsApi.struct_size = sizeof(host.objectsApi);
        host.objectsApi.interface_version = TAO_PLUGIN_OBJECTS_VERSION_1;
        host.a11y.context = [](void* ctx, TaoPluginAccessibilityContextV1* out) {
            auto& s = *static_cast<FakeCueBoard*>(ctx);
            out->board = Handle(TAO_OBJECT_BOARD, 1, s.boardGeneration);
            out->columns = 9; out->rows = 6; out->scene = s.scene; out->flags = s.flags;
            return TAO_PLUGIN_OK;
        };
        host.objectsApi.get_root = [](void*, uint32_t kind, TaoPluginObjectHandle* out) {
            if (kind != TAO_OBJECT_LAWN_APP) return TAO_PLUGIN_NOT_AVAILABLE;
            *out = Handle(kind, 2); return TAO_PLUGIN_OK;
        };
        host.objectsApi.is_valid = [](void*, TaoPluginObjectHandle h, uint8_t* out) {
            *out = h.object_id != 0; return TAO_PLUGIN_OK;
        };
        host.objectsApi.get = [](void* ctx, TaoPluginObjectHandle h, TaoPluginStringView key, TaoPluginValue* out) {
            auto& s = *static_cast<FakeCueBoard*>(ctx);
            const auto name = CopyView(key);
            if (h.kind == TAO_OBJECT_LAWN_APP && name == "game_mode") return Number(out, s.mode);
            if (h.kind == TAO_OBJECT_BOARD) {
                if (name == "background") return Number(out, s.background);
                if (name == "level") return Number(out, s.level);
            }
            if (h.kind == TAO_OBJECT_ZOMBIE && h.object_id >= 100 && h.object_id - 100 < s.zombies.size()) {
                const auto& z = s.zombies[size_t(h.object_id - 100)];
                if (name == "dead") return Boolean(out, z.dead);
                if (name == "age") return Number(out, z.age);
                if (name == "row") return Number(out, z.row);
                if (name == "zombie_type") return Number(out, z.type);
                if (name == "phase" && z.phaseReadable) return Number(out, z.phase);
                if (name == "x" && z.xReadable) return Number(out, z.x);
                if (name == "pos_x" && z.posReadable) {
                    out->kind = TAO_VALUE_NUMBER; out->value.number_value = z.posX;
                    return TAO_PLUGIN_OK;
                }
                if (name == "pos_y") { out->kind = TAO_VALUE_NUMBER; out->value.number_value = 100 + z.row * 100; return TAO_PLUGIN_OK; }
            }
            return TAO_PLUGIN_NOT_AVAILABLE;
        };
        host.objectsApi.call = [](void*, TaoPluginObjectHandle, TaoPluginStringView method,
            const TaoPluginValue* args, size_t, TaoPluginValue* out) {
            if (CopyView(method).find("PixelToGridXKeepOnBoard") != std::string::npos)
                return Number(out, std::clamp((int(args[0].value.integer_value) - 80) / 80, 0, 8));
            return TAO_PLUGIN_NOT_AVAILABLE;
        };
        host.views.open = [](void*, TaoPluginObjectHandle, TaoPluginStringView name, int64_t, TaoPluginViewHandle* out) {
            if (CopyView(name) != "zombies") return TAO_PLUGIN_NOT_AVAILABLE;
            out->view_id = 1; return TAO_PLUGIN_OK;
        };
        host.views.close = [](void*, TaoPluginViewHandle) { return TAO_PLUGIN_OK; };
        host.views.size = [](void* ctx, TaoPluginViewHandle, size_t* out) {
            *out = static_cast<FakeCueBoard*>(ctx)->zombies.size(); return TAO_PLUGIN_OK;
        };
        host.views.get = [](void* ctx, TaoPluginViewHandle, size_t index, TaoPluginValue* out) {
            auto& s = *static_cast<FakeCueBoard*>(ctx);
            if (index >= s.zombies.size()) return TAO_PLUGIN_NOT_AVAILABLE;
            out->kind = TAO_VALUE_OBJECT;
            out->value.object_value = Handle(TAO_OBJECT_ZOMBIE, 100 + index, s.zombies[index].generation);
            return TAO_PLUGIN_OK;
        };
        host.sound.play = [](void* ctx, const TaoPluginAudioPlayDescriptorV1* desc, TaoPluginAudioHandle* out) {
            auto& s = *static_cast<FakeCueBoard*>(ctx);
            s.sounds.push_back({CopyView(desc->resource_id), desc->pan, desc->volume});
            out->audio_id = s.sounds.size(); return TAO_PLUGIN_OK;
        };
        host.sound.destroy = [](void* ctx, TaoPluginAudioHandle sound) {
            static_cast<FakeCueBoard*>(ctx)->destroyed.push_back(sound.audio_id); return TAO_PLUGIN_OK;
        };
        host.sound.describe = [](void* ctx, TaoPluginAudioHandle, TaoPluginAudioInfoV1* out) {
            out->flags = static_cast<FakeCueBoard*>(ctx)->playing ? TAO_AUDIO_PLAYING : 0; return TAO_PLUGIN_OK;
        };
        control.SetObjects(&objects);
        control.SetSay([this](std::string text, bool) { spoken.push_back(std::move(text)); });
    }
    void Update(bool hammerOn = true, bool entryOn = false, bool fastOn = false) {
        const auto ctx = control.Context();
        control.UpdateCues(ctx, tick, entryOn, fastOn, false, false, 5, hammerOn);
        tick += 10;
    }
};

int main() {
    int checks = 0;
    const auto require = [&](bool ok, const char* why) {
        if (!ok) throw std::runtime_error(why);
        ++checks;
    };
    try {
        {
            FakeCueBoard s; s.zombies.resize(1);
            s.Update(); require(s.sounds.empty(), "walking giant is quiet");
            s.zombies[0].phase = 70; s.Update();
            require(s.sounds.size() == 1 && s.sounds.back().id == "SOUND_PVZ_A11Y_HAMMER_H5_R2", "giant raises hammer in row 3");
            require(s.sounds.back().pan == 0, "center giant has centered sound");
            s.Update(); s.Update(); require(s.sounds.size() == 1, "sustained smash does not repeat");
            s.zombies[0].phaseReadable = false; s.Update();
            s.zombies[0].phaseReadable = true; s.Update();
            require(s.sounds.size() == 1, "failed phase read does not rearm smash");
            s.zombies[0].phase = 69; s.Update(); require(s.sounds.size() == 1, "throwing imp is quiet");
            s.zombies[0].phase = 70; s.zombies[0].posX = 200; s.Update();
            require(s.sounds.size() == 2 && s.sounds.back().pan == -5000, "next smash follows giant on left");
            ++s.zombies[0].generation; s.zombies[0].type = 32; s.zombies[0].posX = 600; s.Update();
            require(s.sounds.size() == 3 && s.sounds.back().pan == 5000, "reused slot with red eye giant on right");
            ++s.boardGeneration; s.Update(); require(s.sounds.size() == 4, "new board resets smash state");
        }
        {
            FakeCueBoard s; s.background = 2;
            s.zombies.resize(2);
            s.zombies[0].phase = s.zombies[1].phase = 70;
            s.zombies[0].row = s.zombies[1].row = 5;
            s.zombies[0].posX = 0; s.zombies[1].posX = 800;
            s.zombies[1].type = 32; s.Update();
            require(s.sounds.size() == 2, "same row giants keep separate source positions");
            require(s.sounds[0].id == "SOUND_PVZ_A11Y_HAMMER_H6_R5" && s.sounds[1].id == s.sounds[0].id, "pool sixth row uses six row pitch");
            require(s.sounds[0].pan == -10000 && s.sounds[1].pan == 10000, "opposite sources pan to opposite edges");
        }
        for (int height : {5, 6}) for (int row = 0; row < height; ++row) {
            FakeCueBoard s; s.background = height == 6 ? 3 : 0;
            s.zombies.resize(1); s.zombies[0].phase = 70; s.zombies[0].row = row; s.Update();
            require(s.sounds.size() == 1 && s.sounds[0].id == "SOUND_PVZ_A11Y_HAMMER_H" + std::to_string(height) + "_R" + std::to_string(row), "every playable row selects matching resource");
        }
        {
            FakeCueBoard s; s.zombies.resize(1); s.level = 1; s.zombies[0].phase = 70;
            s.Update(); require(s.sounds.size() == 1 && s.sounds[0].id == "SOUND_PVZ_A11Y_HAMMER_H5_R2", "narrow adventure field retains absolute row pitch");
        }
        {
            FakeCueBoard s; s.zombies.resize(1); s.zombies[0].phase = 70;
            s.Update(false); s.Update(); require(s.sounds.empty(), "enabling during same smash does not replay");
            s.zombies[0].phase = 0; s.Update(); s.zombies[0].phase = 70; s.Update();
            require(s.sounds.size() == 1, "enabling permits next smash");
        }
        {
            FakeCueBoard s; s.zombies.resize(4);
            for (auto& z : s.zombies) z.phase = 70;
            s.zombies[0].type = 0; s.zombies[1].dead = true;
            s.zombies[2].row = -1; s.zombies[3].row = 5;
            s.Update(); require(s.sounds.empty(), "ordinary dead and invalid row zombies are quiet");
        }
        {
            FakeCueBoard s; s.zombies.resize(1); auto& z = s.zombies[0]; z.phase = 70;
            z.posReadable = false; z.x = 600; s.Update();
            require(s.sounds.back().pan == 5000, "integer x is fallback position");
            ++z.generation; z.xReadable = false; s.Update();
            require(s.sounds.back().pan == 0, "unavailable position falls back to center");
            ++z.generation; z.posReadable = true; z.posX = 1200; s.Update();
            require(s.sounds.back().pan == 10000, "offscreen right is bounded");
            ++z.generation; z.posX = -200; s.Update();
            require(s.sounds.back().pan == -10000, "offscreen left is bounded");
            ++z.generation; z.posX = std::numeric_limits<float>::quiet_NaN(); s.Update();
            require(s.sounds.back().pan == 0, "nonfinite position cannot produce invalid pan");
        }
        {
            FakeCueBoard s; s.zombies.resize(2);
            s.zombies[0].type = 3; s.zombies[0].age = 10;
            s.Update(true, true, true);
            require(s.sounds.size() == 1 && s.sounds[0].id == "SOUND_PVZ_A11Y_FAST_H5_R2", "fast cue still plays for new pole vaulter");
            for (int i = 0; i < 4; ++i) s.Update(true, true, true);
            require(s.sounds.size() == 2 && s.sounds[1].id == "SOUND_PVZ_A11Y_ENTRY_H5_R2", "entry queue still plays exactly once");
            s.zombies[1].phase = 70; s.Update(true, true, true);
            require(s.sounds.size() == 3 && s.sounds[2].id == "SOUND_PVZ_A11Y_HAMMER_H5_R2", "hammer cue works alongside entry and fast cues");
        }
        {
            FakeCueBoard s; s.control.SetCueVolumes(30, 40, 60);
            s.zombies.resize(2); s.zombies[0].phase = 70;
            s.zombies[1].type = 3; s.zombies[1].age = 10;
            s.Update(true, true, true);
            require(s.sounds.size() == 2 && s.sounds[0].volume == .3 && s.sounds[1].volume == 1.0, "hammer gain is independent of fast cue");
            for (int i = 0; i < 4; ++i) s.Update(true, true, true);
            require(s.sounds.back().id == "SOUND_PVZ_A11Y_ENTRY_H5_R2" && s.sounds.back().volume == .6, "gameplay entry uses independent entry gain");
            s.control.PreviewHammerCue(); require(s.sounds.back().volume == .3 && s.sounds.back().pan == 0, "hammer preview matches gameplay gain with center pan");
            s.control.SetCueVolumes(75, 40, 60); s.control.PreviewHammerCue();
            require(s.sounds.back().volume == .75 && s.destroyed.back() == s.sounds.size() - 1, "new preview uses adjusted gain and releases previous preview");
            s.control.PreviewCountdownCue(); require(s.sounds.back().id == "SOUND_PVZ_A11Y_COUNTDOWN_5" && s.sounds.back().volume == .4, "countdown preview uses its own waveform and gain");
            s.control.PreviewEntryCue(); require(s.sounds.back().id == "SOUND_PVZ_A11Y_ENTRY_H5_R2" && s.sounds.back().volume == .6, "entry preview uses its own waveform and gain");
            s.control.SetCueVolumes(0, 0, 0); const auto count = s.sounds.size();
            s.control.PreviewHammerCue(); s.control.PreviewCountdownCue(); s.control.PreviewEntryCue();
            require(s.sounds.size() == count, "zero volume preview stays silent");
            ++s.zombies[0].generation; s.Update(); require(s.sounds.size() == count, "zero hammer volume mutes gameplay");
        }
        {
            FakeCueBoard s; s.control.SetCueVolumes(-10, 120, 50);
            s.control.PumpCueSounds(1000); // No Context() or gameplay needed for settings preview.
            s.control.PreviewHammerCue(); require(s.sounds.empty(), "negative hammer volume clamps to mute");
            s.control.PreviewCountdownCue(); require(s.sounds.back().volume == 1.0, "countdown gain clamps to one");
            s.playing = false; s.control.PumpCueSounds(1001);
            require(s.destroyed.size() == 1, "completed preview is reclaimed in modal pump");
            s.control.StopCuePreview(); require(s.destroyed.size() == 1, "completed preview is not destroyed twice");
            s.playing = true; s.control.PreviewEntryCue(); s.control.StopCuePreview();
            require(s.destroyed.size() == 2, "closing settings stops active preview");
            s.control.PreviewEntryCue(); s.control.PumpCueSounds(1402);
            require(s.destroyed.size() == 3, "timeout reclaims preview without gameplay updates");
        }
        {
            FakeCueBoard s; s.control.Context(); s.control.SetRowBrowseOptions(1, true, 45);
            s.zombies.resize(4);
            s.zombies[0].row = s.zombies[1].row = s.zombies[2].row = 1;
            s.zombies[0].posX = 600; s.zombies[1].posX = 200;
            s.zombies[2].dead = true; s.zombies[2].posX = 0; s.zombies[3].row = 2; s.zombies[3].posX = 0;
            s.control.MoveCursor(0, 1); s.control.ReadCell();
            require(s.spoken.size() == 1 && s.spoken.back().ends_with("本行：2 只僵尸"), "row count is combined with cell speech in one utterance");
            require(s.sounds.size() == 1 && s.sounds.back().id == "SOUND_PVZ_A11Y_ROW_H5_R1" && s.sounds.back().pan == -5000 && s.sounds.back().volume == .45,
                "triangle uses row pitch and leftmost live zombie position and independent gain");
            s.control.ReadCell(); require(s.sounds.size() == 1 && s.spoken.back().find("本行：") == std::string::npos, "cell reread does not repeat row notification");
            s.control.MoveCursor(1, 0); s.control.ReadCell(); require(s.sounds.size() == 1, "horizontal movement does not cue row");
            s.control.SetRowBrowseOptions(2, false, 45);
            s.control.MoveCursor(0, 1); s.control.ReadCell();
            const auto detailed = s.spoken.back(); s.control.AnnounceRowZombies();
            require(detailed.ends_with("本行：" + s.spoken.back()), "detailed cross row narration matches Shift exactly");
            require(s.sounds.size() == 1, "sound switch independent of detailed narration");
            s.control.SetRowBrowseOptions(0, true, 60);
            s.control.MoveCursor(0, -1); s.control.ReadCell();
            require(s.sounds.size() == 2 && s.spoken.back().find("本行：") == std::string::npos, "sound only mode leaves ordinary cell speech");
            s.control.MoveCursor(0, -1); s.control.ReadCell(); require(s.sounds.size() == 2, "empty row is silent");
            s.control.MoveCursor(0, -1); s.control.ReadCell(); require(s.sounds.size() == 2, "clamped row boundary is silent");
            s.control.SetRowBrowseOptions(1, false, 60);
            s.control.MoveCursor(0, 1); s.control.ReadCell(); s.control.MoveCursor(0, -1); s.control.ReadCell();
            require(s.spoken.back().ends_with("本行：没有僵尸"), "empty row count is spoken");
        }
        for (int height : {5, 6}) {
            FakeCueBoard s; s.background = height == 6 ? 2 : 0; s.control.Context();
            s.zombies.resize(1); s.zombies[0].row = height - 1; s.zombies[0].posX = 600;
            s.control.SetRowBrowseOptions(0, true, 100); s.control.MoveCursor(0, height - 1); s.control.ReadCell();
            require(s.sounds.back().id == "SOUND_PVZ_A11Y_ROW_H" + std::to_string(height) + "_R" + std::to_string(height - 1) && s.sounds.back().pan == 5000,
                "row browse selects correct five or six row pitch and right pan");
            const auto count = s.sounds.size(); s.control.MoveCursor(0, 1); s.control.ReadCell();
            require(s.sounds.size() == count, "bottom boundary does not repeat notification");
        }
        {
            FakeCueBoard s; s.level = 2; s.control.Context(); s.zombies.resize(1); s.zombies[0].row = 2;
            s.control.MoveCursor(0, 1); s.control.ReadCell();
            require(s.sounds.back().id == "SOUND_PVZ_A11Y_ROW_H5_R2", "narrow field retains absolute row and field height");
        }
        for (int mode : {43, 50}) {
            FakeCueBoard s; s.mode = mode; s.control.Context(); s.zombies.resize(1); s.zombies[0].row = 1;
            s.control.MoveCursor(0, 1); s.control.ReadCell(); require(s.sounds.empty(), "garden and wisdom tree do not announce row zombies");
        }
        {
            FakeCueBoard s; s.control.Context(); s.zombies.resize(1); s.zombies[0].row = 1;
            s.zombies[0].posX = std::numeric_limits<float>::quiet_NaN();
            s.control.MoveCursor(0, 1); s.control.ReadCell();
            require(s.sounds.empty() && s.spoken.back().ends_with("本行：1 只僵尸"), "missing position preserves count without inventing pan");
            s.control.SetRowBrowseOptions(0, false, 75); s.control.PreviewRowBrowseCue();
            require(s.sounds.back().id == "SOUND_PVZ_A11Y_ROW_H5_R2" && s.sounds.back().volume == .75 && s.sounds.back().pan == 0, "settings preview works while row cue is disabled");
            s.control.SetRowBrowseOptions(0, true, 0); const auto count = s.sounds.size();
            s.control.PreviewRowBrowseCue(); require(s.sounds.size() == count, "zero row preview volume is silent");
            s.flags = TAO_INPUT_MODAL; s.zombies[0].row = 2; s.zombies[0].posX = 400;
            s.control.SetRowBrowseOptions(1, true, 100); s.control.MoveCursor(0, 1); s.control.ReadCell();
            require(s.sounds.size() == count && s.spoken.back().find("本行：") == std::string::npos, "modal movement cannot trigger row prompt");
        }
        std::cout << "Passed " << checks << " gargantuar and row cue checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
