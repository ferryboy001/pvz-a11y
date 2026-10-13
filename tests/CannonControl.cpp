// SPDX-License-Identifier: LGPL-3.0-or-later
// Runs BoardControl against a fake versioned host; no game or player save required.
#define NOMINMAX
#include <windows.h>
#undef GetMessage
#include "BoardControl.hpp"
#include "InputMap.hpp"
#include <iostream>
#include <stdexcept>

extern "C" void* TaoEngineResolveObject(TaoPluginObjectHandle) noexcept { return nullptr; }
extern "C" void* TaoEngineGetApp() noexcept { return nullptr; }
std::string __cdecl TodStringTranslate(std::string_view text) { return std::string(text); }

struct FakeBoard {
    struct Plant {
        int type = 47, state = 37, column = 0, row = 0, width = 2;
        bool dead = false, squished = false, asleep = false, onBoard = true;
        int bungee = 0;
        bool acceptsClick = true, stale = false;
    };
    HostServices host{};
    tao::Objects objects;
    BoardControl control;
    std::vector<Plant> plants;
    struct Vase {
        int state = 3, content = 1, seed = 0, zombie = 0, transparent = 0;
        bool dead = false, readable = true;
    };
    std::vector<Vase> vases;
    int cursor = 0, selected = -1, shots = 0, returns = 0, clicks = 0;
    int targetColumn = -1, targetRow = -1, lastShot = -1;
    int timeStop = 0, scene = 3, mode = 0;
    uint32_t flags = 0;
    bool allowed = true, paused = false, rejectShot = false;
    std::string spoken;

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
    static TaoPluginStatus Object(TaoPluginValue* out, TaoPluginObjectHandle h) {
        out->kind = TAO_VALUE_OBJECT; out->value.object_value = h; return TAO_PLUGIN_OK;
    }
    FakeBoard() : objects(host.objectsApi), control(&host) {
        host.a11y.host_context = host.objectsApi.host_context = host.views.host_context = host.board.host_context = this;
        host.objectsApi.struct_size = sizeof(host.objectsApi);
        host.objectsApi.interface_version = TAO_PLUGIN_OBJECTS_VERSION_1;
        host.a11y.context = [](void* ctx, TaoPluginAccessibilityContextV1* out) {
            auto& s = *static_cast<FakeBoard*>(ctx);
            out->board = Handle(TAO_OBJECT_BOARD, 1);
            out->columns = 9; out->rows = 5; out->scene = s.scene;
            out->cursor = s.cursor; out->flags = s.flags;
            return TAO_PLUGIN_OK;
        };
        host.objectsApi.get_root = [](void*, uint32_t kind, TaoPluginObjectHandle* out) {
            if (kind != TAO_OBJECT_LAWN_APP) return TAO_PLUGIN_NOT_AVAILABLE;
            *out = Handle(kind, 2); return TAO_PLUGIN_OK;
        };
        host.objectsApi.get = [](void* ctx, TaoPluginObjectHandle h, TaoPluginStringView key, TaoPluginValue* out) {
            auto& s = *static_cast<FakeBoard*>(ctx);
            const auto name = CopyView(key);
            if (h.kind == TAO_OBJECT_LAWN_APP && name == "game_mode") return Number(out, s.mode);
            if (h.kind == TAO_OBJECT_BOARD) {
                if (name == "background") return Number(out, 0);
                if (name == "level") return Number(out, 20);
                if (name == "paused") return Boolean(out, s.paused);
                if (name == "time_stop_counter") return Number(out, s.timeStop);
                return TAO_PLUGIN_NOT_AVAILABLE;
            }
            if (h.kind == TAO_OBJECT_GRID_ITEM && h.object_id >= 200 && h.object_id - 200 < s.vases.size()) {
                const auto& v = s.vases[size_t(h.object_id - 200)];
                if (name == "dead") return Boolean(out, v.dead);
                if (name == "grid_x" || name == "grid_y") return Number(out, 0);
                if (name == "type") return Number(out, 7);
                if (name == "state") return Number(out, v.state);
                if (!v.readable) return TAO_PLUGIN_NOT_AVAILABLE;
                if (name == "scary_pot_type") return Number(out, v.content);
                if (name == "transparent_counter") return Number(out, v.transparent);
                if (name == "seed_type") return Number(out, v.seed);
                if (name == "zombie_type") return Number(out, v.zombie);
                return TAO_PLUGIN_NOT_AVAILABLE;
            }
            if (h.kind != TAO_OBJECT_PLANT || h.object_id < 100 || h.object_id - 100 >= s.plants.size())
                return TAO_PLUGIN_STALE_HANDLE;
            const auto& p = s.plants[size_t(h.object_id - 100)];
            if (p.stale) return TAO_PLUGIN_STALE_HANDLE;
            if (name == "state") return Number(out, p.state);
            if (name == "row") return Number(out, p.row);
            if (name == "column") return Number(out, p.column);
            if (name == "dead") return Boolean(out, p.dead);
            if (name == "squished") return Boolean(out, p.squished);
            if (name == "asleep") return Boolean(out, p.asleep);
            if (name == "on_board") return Boolean(out, p.onBoard);
            if (name == "bungee_state") return Number(out, p.bungee);
            return TAO_PLUGIN_NOT_AVAILABLE;
        };
        host.objectsApi.call = [](void* ctx, TaoPluginObjectHandle h, TaoPluginStringView method,
            const TaoPluginValue* args, size_t, TaoPluginValue* out) {
            auto& s = *static_cast<FakeBoard*>(ctx);
            const auto name = CopyView(method);
            if (h.kind == TAO_OBJECT_BOARD) {
                if (name == "can_interact_with_buttons") return Boolean(out, s.allowed);
                if (name == "refresh_seed_packet") { ++s.returns; s.cursor = 0; return TAO_PLUGIN_OK; }
                if (name == "clear_cursor") { s.cursor = 0; s.selected = -1; return TAO_PLUGIN_OK; }
                if (name == "get_top_plant_at") {
                    for (size_t i = 0; i < s.plants.size(); ++i) {
                        const auto& p = s.plants[i];
                        if (!p.dead && p.row == args[1].value.integer_value &&
                            args[0].value.integer_value >= p.column && args[0].value.integer_value < p.column + p.width)
                            return Object(out, Handle(TAO_OBJECT_PLANT, 100 + i));
                    }
                    return Object(out, Handle(TAO_OBJECT_PLANT, 0));
                }
            }
            if (h.kind == TAO_OBJECT_PLANT && h.object_id >= 100 && h.object_id - 100 < s.plants.size()) {
                auto& p = s.plants[size_t(h.object_id - 100)];
                if (p.stale) return TAO_PLUGIN_STALE_HANDLE;
                if (name == "Lawn.Plant.GetPlantType() const") return Number(out, p.type);
                if (name == "get_rect") {
                    out->kind = TAO_VALUE_RECT;
                    out->value.rect_value = {80 + p.column * 80, 80 + p.row * 100, p.width * 80, 100};
                    return TAO_PLUGIN_OK;
                }
                if (name == "Lawn.Plant.MouseDown(int, int, int)") {
                    ++s.clicks;
                    if (p.acceptsClick) { s.cursor = 8; s.selected = int(h.object_id - 100); }
                    return TAO_PLUGIN_OK;
                }
            }
            return TAO_PLUGIN_NOT_AVAILABLE;
        };
        host.views.open = [](void*, TaoPluginObjectHandle, TaoPluginStringView name, int64_t, TaoPluginViewHandle* out) {
            if (CopyView(name) == "plants") out->view_id = 1;
            else if (CopyView(name) == "grid_items") out->view_id = 2;
            else return TAO_PLUGIN_NOT_AVAILABLE;
            return TAO_PLUGIN_OK;
        };
        host.views.close = [](void*, TaoPluginViewHandle) { return TAO_PLUGIN_OK; };
        host.views.size = [](void* ctx, TaoPluginViewHandle view, size_t* out) {
            auto& s = *static_cast<FakeBoard*>(ctx);
            *out = view.view_id == 2 ? s.vases.size() : s.plants.size(); return TAO_PLUGIN_OK;
        };
        host.views.get = [](void*, TaoPluginViewHandle view, size_t i, TaoPluginValue* out) {
            return view.view_id == 2 ? Object(out, Handle(TAO_OBJECT_GRID_ITEM, 200 + i)) :
                Object(out, Handle(TAO_OBJECT_PLANT, 100 + i));
        };
        host.board.invoke = [](void* ctx, const TaoPluginBoardActionRequestV2* req, TaoPluginBoardActionResultV2* out) {
            auto& s = *static_cast<FakeBoard*>(ctx);
            if (req->action != TAO_BOARD_FIRE_CANNON) throw std::runtime_error("unexpected board action");
            if (!s.allowed || s.paused || s.timeStop || s.flags & TAO_INPUT_MODAL) {
                out->reason = TAO_ACTION_BLOCKED; return TAO_PLUGIN_ERROR;
            }
            if (s.rejectShot || s.cursor != 8 || s.selected < 0 || s.plants[size_t(s.selected)].dead ||
                s.plants[size_t(s.selected)].state != 37) {
                out->reason = TAO_ACTION_NO_SELECTION; return TAO_PLUGIN_ERROR;
            }
            ++s.shots; s.lastShot = s.selected;
            s.targetColumn = req->column; s.targetRow = req->row;
            s.plants[size_t(s.selected)].state = 38;
            s.cursor = 0; s.selected = -1;
            return TAO_PLUGIN_OK;
        };
        control.SetObjects(&objects);
        control.SetSay([this](std::string text, bool) { spoken = std::move(text); });
        control.Context();
    }
};

int main() {
    int checks = 0;
    auto require = [&](bool ok, const char* message) { if (!ok) throw std::runtime_error(message); ++checks; };
    {
        InputMap input; Action action{}; int param = 0;
        require(input.Find(0, 'C', false, action, param) && action == Action::FireCannon, "C binding");
        require(!input.Find(0, 'C', true, action, param), "holding C must not fire repeatedly");
        require(input.Find(0, 'S', false, action, param) && action == Action::SlotPull, "slot binding preserved");
    }
    for (int type : {47, -1, 125}) {
        FakeBoard s; s.plants.push_back({type});
        s.control.MoveCursor(1, 0); // second occupied cell of a two-cell cannon
        require(s.control.TryCannonConfirm() && s.cursor == 8 && s.shots == 0, "Enter activates without firing");
        s.control.MoveCursor(5, 3);
        require(s.control.TryCannonConfirm() && s.shots == 1 && s.cursor == 0, "second Enter fires");
        require(s.targetColumn == 6 && s.targetRow == 3, "Enter uses target cell");
        s.plants[0].state = 37;
        s.control.MoveCursor(-6, -3); s.control.TryCannonConfirm(); s.control.ReturnHeldItem();
        require(s.cursor == 0 && s.shots == 1 && s.spoken == "已取消炮击瞄准", "Backspace cancels");
    }
    {
        FakeBoard s;
        s.plants = {{47, 37, 2, 2}, {-1, 37, 4, 0}, {47, 36, 0, 0}, {47, 37, 1, 0}};
        s.control.MoveCursor(8, 4); s.control.FireReadyCannon();
        require(s.shots == 1 && s.lastShot == 3, "C fires one cannon in row/column order");
        require(s.targetColumn == 8 && s.targetRow == 4, "C preserves target during selection");
        s.control.FireReadyCannon();
        require(s.shots == 2 && s.lastShot == 1, "next C skips fired and charging cannon");
    }
    for (int exclusion = 0; exclusion < 8; ++exclusion) {
        FakeBoard s; s.plants.push_back({}); auto& p = s.plants[0];
        switch (exclusion) {
        case 0: p.state = 36; break; case 1: p.dead = true; break;
        case 2: p.squished = true; break; case 3: p.asleep = true; break;
        case 4: p.onBoard = false; break; case 5: p.bungee = 1; break;
        case 6: p.stale = true; break; case 7: p.state = 1; p.type = 0; break;
        }
        s.cursor = 1; s.control.FireReadyCannon();
        require(s.shots == 0 && s.cursor == 1 && s.returns == 0 && s.clicks == 0,
            "unavailable candidates must not alter held card");
        require(s.spoken == "没有可发射的玉米炮", "no available cannon feedback");
    }
    {
        FakeBoard s; s.control.FireReadyCannon();
        require(s.spoken == "没有可发射的玉米炮", "empty battlefield feedback");
        s.plants = {{47, 36}};
        require(s.control.TryCannonConfirm() && s.clicks == 0, "charging Enter consumed without planting");
        s.plants = {{0, 1}};
        require(!s.control.TryCannonConfirm(), "ordinary plant Enter falls through");
        s.plants = {{47}}; s.cursor = 1;
        require(!s.control.TryCannonConfirm() && s.cursor == 1, "held-card Enter remains planting");
    }
    for (int gate = 0; gate < 4; ++gate) {
        FakeBoard s; s.plants.push_back({});
        if (gate == 0) s.allowed = false;
        if (gate == 1) s.paused = true;
        if (gate == 2) s.timeStop = 1;
        if (gate == 3) s.flags = TAO_INPUT_MODAL;
        s.control.FireReadyCannon();
        require(s.shots == 0 && s.clicks == 0 && s.returns == 0, "blocked board must not mutate selection");
    }
    {
        FakeBoard s; s.plants = {{47, 37, 0, 0}, {-1, 37, 2, 0}};
        s.plants[0].acceptsClick = false; s.control.MoveCursor(7, 2); s.control.FireReadyCannon();
        require(s.shots == 1 && s.lastShot == 1, "false-success click skips to next ready cannon");
        s.plants[1].state = 37; s.control.MoveCursor(-5, -2); s.control.TryCannonConfirm();
        s.plants[1].dead = true; s.control.MoveCursor(4, 1);
        require(s.control.TryCannonConfirm() && s.shots == 1 && s.cursor == 8,
            "destroyed selected cannon must not fire or plant");
        s.control.ReturnHeldItem(); require(s.cursor == 0, "stale aim can be canceled");
    }
    {
        FakeBoard s; s.vases.push_back({});
        const auto read = [&] { s.control.ReadCell(); return s.spoken; };
        require(read() == "花瓶，A1", "opaque question vase must not disclose plant content");
        auto& v = s.vases[0]; v.content = 2;
        require(read() == "花瓶，A1", "opaque question vase must not disclose zombie content");
        v.state = 4;
        require(read() == "植物花瓶，A1", "leaf shell uses visible category rather than hidden content");
        v.state = 5;
        require(read() == "僵尸花瓶，A1", "zombie shell uses visible category");
        v.state = 3; v.transparent = 1; v.content = 1;
        require(read() == "豌豆射手花瓶，A1", "lit seed vase reads plant name");
        v.transparent = 50; v.content = 3;
        require(read() == "阳光花瓶，A1", "fully transparent sun vase");
        v.content = 2;
        require(read().find("僵尸花瓶，A1") != std::string::npos, "lit zombie vase reads zombie name");
        v.transparent = 0;
        require(read() == "花瓶，A1", "lighting removal restores hidden contents");
        v.readable = false; v.state = 4;
        require(read() == "植物花瓶，A1", "unavailable content fields preserve visible shell");
        v.dead = true;
        require(read() == "草坪，A1", "broken vase must disappear from cell reading");
    }
    std::cout << checks << " cannon and vase checks passed\n";
}
