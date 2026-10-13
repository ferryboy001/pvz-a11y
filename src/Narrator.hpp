// SPDX-License-Identifier: LGPL-3.0-or-later
// 无障碍事件订阅与轮询（场景/弹窗/焦点/文本变化）。
// 事件经回调交给上层，本模块不决定朗读策略。
#pragma once
#include "HostServices.hpp"
#include <functional>
#include <string>

class Narrator {
public:
    struct Handlers {
        std::function<void(std::string text)> scene;
        // 弹窗打开：额外携带事件节点（宿主弹窗控件句柄），供控件导航锚定弹窗子树
        //（弹窗不在 widgets.root_* 里，只按根控件遍历会漏掉商店等 Dialog）。
        std::function<void(TaoPluginAccessibleNodeV2 node, std::string text)> dialogOpen;
        std::function<void()> dialogClose;
        std::function<void(TaoPluginAccessibleNodeV2 node, std::string text)> focus;
        std::function<void(std::string text)> text;
        std::function<void(uint32_t lost)> lost;
    };

    void SetHandlers(Handlers value) { handlers = std::move(value); }

    bool Subscribe(const TaoPluginAccessibilityV1& a11y) {
        api = &a11y;
        return a11y.subscribe(a11y.host_context, (UINT64_C(1) << (TAO_A11Y_STATE + 1)) - 2, &token) == TAO_PLUGIN_OK;
    }
    void Pump() {
        if (!api || !token) return;
        // 每 tick 上限：事件处理可能触发全树遍历（弹窗打开→Nodes()），突发事件
        // 一次吃完会叠爆 OnUpdate 预算（宿主超时即永久禁用回调）。积压下帧继续。
        for (int i = 0; i < 12; ++i) {
            TaoPluginAccessibilityEventV1 event{};
            event.struct_size = sizeof(event);
            if (api->poll(api->host_context, token, &event) != TAO_PLUGIN_OK) break;
            if (event.lost_events && handlers.lost) handlers.lost(event.lost_events);
            std::string text = CopyView(event.text);
            switch (event.kind) {
            case TAO_A11Y_SCENE: if (handlers.scene) handlers.scene(std::move(text)); break;
            case TAO_A11Y_DIALOG_OPEN: if (handlers.dialogOpen) handlers.dialogOpen(event.node, std::move(text)); break;
            case TAO_A11Y_DIALOG_CLOSE: if (handlers.dialogClose) handlers.dialogClose(); break;
            case TAO_A11Y_FOCUS: if (handlers.focus) handlers.focus(event.node, std::move(text)); break;
            default: if (!text.empty() && handlers.text) handlers.text(std::move(text)); break;
            }
        }
    }
    void Unsubscribe() {
        if (api && token) api->unsubscribe(api->host_context, token);
        token = 0;
    }

private:
    const TaoPluginAccessibilityV1* api = nullptr;
    TaoPluginToken token = 0;
    Handlers handlers;
};
