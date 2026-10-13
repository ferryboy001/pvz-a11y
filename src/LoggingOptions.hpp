// SPDX-License-Identifier: LGPL-3.0-or-later
// 宿主 Mod 配置中的日志开关，独立于无障碍启用状态与玩家进度设置。
#pragma once
#include <tao/TaoCppCore.hpp>
#include "TraceLog.hpp"
#include <atomic>
#include <iterator>

class LoggingOptions {
public:
    bool Connect(const TaoPluginHostV2* host) {
        if (!host || tao::QueryInterface(*host, TAO_PLUGIN_CONFIG_INTERFACE,
                TAO_PLUGIN_CONFIG_VERSION_1, api) != TAO_PLUGIN_OK ||
            api.struct_size < sizeof(api) || api.interface_version != TAO_PLUGIN_CONFIG_VERSION_1 ||
            !api.define_schema || !api.get) return false;
        const TaoPluginConfigFieldV1 fields[] = {
            Field("trace_log", "启用 Trace 日志（mods/trace.log），用于记录控件操作与调试追踪。"),
            Field("mod_log", "启用 Mod 日志：将 pvz-a11y 的朗读与诊断内容写入 mods/mod.log。关闭不影响语音。")
        };
        if (api.define_schema(api.host_context, 1, fields, std::size(fields)) != TAO_PLUGIN_OK) return false;
        ready = true;
        Refresh();
        return true;
    }
    void Refresh() {
        if (!ready) return;
        tracefile::SetEnabled(Read("trace_log", tracefile::Enabled()));
        modEnabled.store(Read("mod_log", ModEnabled()), std::memory_order_relaxed);
    }
    bool ModEnabled() const { return modEnabled.load(std::memory_order_relaxed); }
    bool Set(const char* key, bool enabled) {
        if (!ready || !api.set) return false;
        TaoPluginValue value{};
        value.struct_size = sizeof(value); value.kind = TAO_VALUE_BOOLEAN;
        value.value.boolean_value = enabled;
        if (api.set(api.host_context, tao::AbiString(key), &value) != TAO_PLUGIN_OK) return false;
        Refresh();
        return true;
    }

private:
    static TaoPluginConfigFieldV1 Field(const char* key, const char* description) {
        TaoPluginConfigFieldV1 field{};
        field.struct_size = sizeof(field);
        field.key = tao::AbiString(key);
        field.description = tao::AbiString(description);
        field.default_value.struct_size = sizeof(TaoPluginValue);
        field.default_value.kind = TAO_VALUE_BOOLEAN;
        field.default_value.value.boolean_value = 1;
        field.minimum_value.struct_size = field.maximum_value.struct_size = sizeof(TaoPluginValue);
        field.minimum_value.kind = field.maximum_value.kind = TAO_VALUE_NIL;
        return field;
    }
    bool Read(const char* key, bool fallback) const {
        TaoPluginValue value{};
        value.struct_size = sizeof(value);
        if (api.get(api.host_context, tao::AbiString(key), &value) != TAO_PLUGIN_OK ||
            value.kind != TAO_VALUE_BOOLEAN || value.value.boolean_value > 1) return fallback;
        return value.value.boolean_value != 0;
    }
    TaoPluginConfigV1 api{};
    bool ready = false;
    std::atomic<bool> modEnabled{true};
};
