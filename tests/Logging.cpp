// SPDX-License-Identifier: LGPL-3.0-or-later
#include "LoggingPanel.hpp"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>

struct FakeConfig {
    TaoPluginHostV2 host{};
    TaoPluginConfigV1 api{};
    bool trace = false, mod = false, failRead = false, failWrite = false;
    int definitions = 0, writes = 0;
    FakeConfig() {
        host.struct_size = sizeof(host); host.host_context = this;
        api.struct_size = sizeof(api); api.interface_version = TAO_PLUGIN_CONFIG_VERSION_1; api.host_context = this;
        api.define_schema = [](void* ctx, uint32_t version, const TaoPluginConfigFieldV1* fields, size_t count) {
            auto& s = *static_cast<FakeConfig*>(ctx);
            if (version != 1 || count != 2) return TAO_PLUGIN_INVALID_ARGUMENT;
            const char* expected[] = {"trace_log", "mod_log"};
            for (size_t i = 0; i < count; ++i) {
                if (std::string_view(fields[i].key.data, fields[i].key.size) != expected[i] ||
                    fields[i].default_value.kind != TAO_VALUE_BOOLEAN || !fields[i].default_value.value.boolean_value ||
                    fields[i].description.size == 0 || fields[i].flags != 0) return TAO_PLUGIN_INVALID_ARGUMENT;
            }
            ++s.definitions; return TAO_PLUGIN_OK;
        };
        api.get = [](void* ctx, TaoPluginStringView key, TaoPluginValue* out) {
            auto& s = *static_cast<FakeConfig*>(ctx);
            if (s.failRead) return TAO_PLUGIN_NOT_AVAILABLE;
            out->kind = TAO_VALUE_BOOLEAN;
            out->value.boolean_value = std::string_view(key.data, key.size) == "trace_log" ? s.trace : s.mod;
            return TAO_PLUGIN_OK;
        };
        // 不提供 set/reset：加载配置只能读，不能覆盖已保存的开关。
        host.query_interface = [](void* ctx, TaoPluginStringView name, uint32_t version, void* out, size_t size) {
            auto& s = *static_cast<FakeConfig*>(ctx);
            if (std::string_view(name.data, name.size) != TAO_PLUGIN_CONFIG_INTERFACE ||
                version != TAO_PLUGIN_CONFIG_VERSION_1 || size != sizeof(s.api)) return TAO_PLUGIN_NOT_AVAILABLE;
            std::memcpy(out, &s.api, sizeof(s.api)); return TAO_PLUGIN_OK;
        };
    }
};

struct PanelHost {
    TaoPluginHostV2 host{};
    TaoPluginUiV1 ui{};
    TaoPluginUiControlsV1 controls{};
    bool open = false;
    std::string focused;
    std::map<std::string, std::string> texts;
    std::vector<std::pair<std::string, std::string>> events;
    size_t eventIndex = 0;
    static std::string Copy(TaoPluginStringView value) { return {value.data, value.size}; }
    PanelHost() {
        host.host_context = ui.host_context = controls.host_context = this;
        ui.struct_size = sizeof(ui); ui.interface_version = TAO_PLUGIN_UI_VERSION_1;
        controls.struct_size = sizeof(controls); controls.interface_version = TAO_PLUGIN_UI_CONTROLS_VERSION_1;
        ui.mount = [](void* ctx, const TaoPluginUiNodeV1* nodes, size_t count, uint32_t flags, TaoPluginToken* token) {
            auto& s = *static_cast<PanelHost*>(ctx);
            if (!(flags & TAO_UI_MODAL)) return TAO_PLUGIN_INVALID_ARGUMENT;
            s.open = true; *token = 1;
            for (size_t i = 0; i < count; ++i) s.texts[Copy(nodes[i].key)] = Copy(nodes[i].text);
            return TAO_PLUGIN_OK;
        };
        ui.close = [](void* ctx, TaoPluginToken) { static_cast<PanelHost*>(ctx)->open = false; return TAO_PLUGIN_OK; };
        ui.is_valid = [](void* ctx, TaoPluginToken, uint8_t* out) { *out = static_cast<PanelHost*>(ctx)->open; return TAO_PLUGIN_OK; };
        ui.poll = [](void*, TaoPluginToken, TaoPluginUiEventV1*) { return TAO_PLUGIN_OK; };
        ui.set_text = [](void* ctx, TaoPluginToken, TaoPluginStringView key, TaoPluginStringView text) {
            static_cast<PanelHost*>(ctx)->texts[Copy(key)] = Copy(text); return TAO_PLUGIN_OK;
        };
        ui.set_visible = ui.set_enabled = [](void*, TaoPluginToken, TaoPluginStringView, uint8_t) { return TAO_PLUGIN_OK; };
        controls.state = [](void* ctx, TaoPluginToken, TaoPluginStringView key, TaoPluginUiControlStateV1* out) {
            out->text = tao::AbiString(static_cast<PanelHost*>(ctx)->texts[Copy(key)]); return TAO_PLUGIN_OK;
        };
        controls.poll = [](void* ctx, TaoPluginToken, TaoPluginUiControlEventV1* out) {
            auto& s = *static_cast<PanelHost*>(ctx);
            if (s.eventIndex < s.events.size()) {
                const auto& event = s.events[s.eventIndex++]; out->event.available = 1;
                out->event.key = tao::AbiString("trace_log"); out->event.name = tao::AbiString(event.first);
                out->text = tao::AbiString(event.second);
            }
            return TAO_PLUGIN_OK;
        };
        controls.layout = [](void*, TaoPluginToken, TaoPluginStringView, int32_t, int32_t) { return TAO_PLUGIN_OK; };
        controls.scroll = [](void*, TaoPluginToken, TaoPluginStringView, int32_t) { return TAO_PLUGIN_OK; };
        controls.focus = [](void* ctx, TaoPluginToken, TaoPluginStringView key) {
            static_cast<PanelHost*>(ctx)->focused = Copy(key); return TAO_PLUGIN_OK;
        };
        host.query_interface = [](void* ctx, TaoPluginStringView name, uint32_t, void* out, size_t size) {
            auto& s = *static_cast<PanelHost*>(ctx);
            if (Copy(name) == TAO_PLUGIN_UI_INTERFACE && size == sizeof(s.ui)) { std::memcpy(out, &s.ui, size); return TAO_PLUGIN_OK; }
            if (Copy(name) == TAO_PLUGIN_UI_CONTROLS_INTERFACE && size == sizeof(s.controls)) { std::memcpy(out, &s.controls, size); return TAO_PLUGIN_OK; }
            return TAO_PLUGIN_NOT_AVAILABLE;
        };
    }
};

int main() {
    int checks = 0;
    const auto require = [&](bool ok, const char* why) { if (!ok) throw std::runtime_error(why); ++checks; };
    try {
        FakeConfig config;
        LoggingOptions options;
        require(options.Connect(&config.host) && config.definitions == 1, "schema must expose two editable boolean fields");
        require(!options.ModEnabled() && !tracefile::Enabled(), "persisted disabled settings must apply before startup logs");
        config.trace = true; options.Refresh();
        require(tracefile::Enabled() && !options.ModEnabled(), "trace can be enabled independently");
        config.trace = false; config.mod = true; options.Refresh();
        require(!tracefile::Enabled() && options.ModEnabled(), "mod logging can be enabled independently");
        config.mod = false; options.Refresh(); config.failRead = true; options.Refresh();
        require(!tracefile::Enabled() && !options.ModEnabled(), "transient read failure must retain disabled settings");
        config.failRead = false; config.mod = config.trace = true; options.Refresh();
        require(tracefile::Enabled() && options.ModEnabled(), "reset/enabling both must resume logs");
        require(!options.Set("trace_log", false), "missing writer fails without changing active setting");
        config.api.set = [](void* ctx, TaoPluginStringView key, const TaoPluginValue* value) {
            auto& s = *static_cast<FakeConfig*>(ctx);
            if (s.failWrite) return TAO_PLUGIN_NOT_AVAILABLE;
            if (std::string_view(key.data, key.size) == "trace_log") s.trace = value->value.boolean_value != 0;
            else s.mod = value->value.boolean_value != 0;
            ++s.writes; return TAO_PLUGIN_OK;
        };
        LoggingOptions editable;
        require(editable.Connect(&config.host) && config.writes == 0, "connecting must not overwrite persisted settings");
        require(editable.Set("trace_log", false) && !tracefile::Enabled() && editable.ModEnabled() && config.writes == 1,
            "panel save persists one setting and applies immediately");
        config.failWrite = true;
        require(!editable.Set("mod_log", false) && editable.ModEnabled(), "failed save preserves previous live setting");
        config.failWrite = false;
        require(editable.Set("mod_log", false) && !editable.ModEnabled() && !tracefile::Enabled(), "both logs can be disabled through config writer");
        PanelHost panelHost;
        tao::UiController controller; controller.Attach(panelHost.host, tao::Logger{});
        LoggingPanel panel; int closes = 0; std::vector<std::string> spoken;
        panel.Connect(controller, editable, [&](std::string text) { spoken.push_back(std::move(text)); }, [&] { ++closes; });
        panel.RequestOpen(); panel.Pump();
        require(panel.Active() && panelHost.focused == "trace_log" && panelHost.texts["trace_log"].starts_with("0\t"), "opening panel presents persisted values and first focus");
        panel.Queue(Action::Option); panel.Pump();
        require(panelHost.focused == "mod_log", "Tab moves to mod log");
        panel.Queue(Action::Confirm); panel.Pump();
        require(editable.ModEnabled() && !tracefile::Enabled() && panelHost.texts["mod_log"].starts_with("1\t"), "keyboard saves and updates visible checkbox independently");
        panel.Queue(Action::Option, -1); panel.Pump();
        require(panelHost.focused == "trace_log", "Shift Tab restores previous field");
        const int writes = config.writes;
        panelHost.events = {{"click", ""}, {"change", "1\t启用 Trace 日志"}};
        controller.Pump(); panel.Pump();
        require(tracefile::Enabled() && config.writes == writes + 1, "mouse click/change pair saves once");
        panel.Queue(Action::Start); panel.Pump();
        require(!panel.Active() && closes == 1, "Escape closes and invokes focus return hook");
        panel.RequestOpen(); panel.Pump();
        require(panelHost.texts["trace_log"].starts_with("1\t") && panelHost.texts["mod_log"].starts_with("1\t"), "reopening uses saved values");
        panelHost.open = false; panel.Pump();
        require(closes == 2, "native close also returns previous focus");

        // 验证 wave_countdown 开关在面板中的行为
        bool countdownState = true; int countdownSets = 0;
        LoggingPanel panel2; int closes2 = 0;
        panel2.Connect(controller, editable, [&](std::string text) { spoken.push_back(std::move(text)); }, [&] { ++closes2; },
            [&] { return countdownState; },
            [&](bool val) { countdownState = val; ++countdownSets; return true; });
        panel2.RequestOpen(); panel2.Pump();
        require(panel2.Active() && panelHost.texts["wave_countdown"].starts_with("1\t"), "panel initializes wave_countdown from hook");
        panel2.Queue(Action::Option); panel2.Pump(); // mod_log
        panel2.Queue(Action::Option); panel2.Pump(); // wave_countdown
        require(panelHost.focused == "wave_countdown", "Tab reaches wave_countdown");
        panel2.Queue(Action::Confirm); panel2.Pump();
        require(!countdownState && countdownSets == 1 && panelHost.texts["wave_countdown"].starts_with("0\t"), "Confirm toggles wave_countdown off");
        panel2.Queue(Action::Option); panel2.Pump();
        require(panelHost.focused == "close", "Tab reaches close button");
        panel2.Queue(Action::Confirm); panel2.Pump();
        require(!panel2.Active() && closes2 == 1, "Confirm on close button closes panel");

        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto directory = std::filesystem::path(executable).parent_path() / "mods";
        std::filesystem::create_directories(directory);
        const auto file = directory / "trace.log";
        const auto size = [&] { return std::filesystem::exists(file) ? std::filesystem::file_size(file) : 0; };
        const auto before = size();
        tracefile::SetEnabled(false); tracefile::Write("disabled logging regression marker");
        require(size() == before, "disabled trace must not create or append to trace.log");
        tracefile::SetEnabled(true); tracefile::Write("enabled logging regression marker");
        require(size() > before, "enabled trace must append normally");
        std::cout << checks << " logging configuration checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
