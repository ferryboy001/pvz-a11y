// SPDX-License-Identifier: LGPL-3.0-or-later
// 宿主语义服务连接层。所有与游戏的交互都经由这些版本化 ABI 表完成，
// 不涉及任何内存读写或字节补丁（整体取代 PvZA11y 的 MemoryIO.cs）。
#pragma once
#include <tao/Tao.hpp>
#include <tao/TaoGameplayAbi.h>
#include <atomic>
#include <string>

struct HostServices {
    TaoPluginObjectsV1 objectsApi{};
    TaoPluginAccessibilityV1 a11y{};
    TaoPluginAccessibleUiV2 ui{};
    TaoPluginWidgetsV1 widgets{};
    TaoPluginBoardActionsV2 board{};
    TaoPluginSimulationV1 simulation{};
    TaoPluginAudioGroupV1 audio{};
    TaoPluginAudioV1 sound{}; // 可选：提示音播放（tao.audio）；缺失时提示音静默跳过
    TaoPluginInputEventsV2 input{};
    TaoPluginViewsV1 views{};
    // 可选增强：definition 原版定义查询与 methods 静态方法调用（名称运行时解析用）。
    // 查询失败不影响主体功能，调用方判空后回退内置名称表。
    TaoPluginGameplayV1 gameplay{};
    TaoPluginMethodsV2 methods{};
    // 可选：场景流转（打开商店、查询是否可开店）。缺失时商店入口退化为只提示不可用。
    TaoPluginNavigationV1 navigation{};

    bool Connect(const TaoPluginHostV2* host) {
        if (!(Query(host, TAO_PLUGIN_OBJECTS_INTERFACE, TAO_PLUGIN_OBJECTS_VERSION_1, objectsApi)
            && Query(host, TAO_PLUGIN_ACCESSIBILITY_INTERFACE, TAO_PLUGIN_ACCESSIBILITY_VERSION_1, a11y)
            && Query(host, TAO_PLUGIN_ACCESSIBLE_UI_INTERFACE, TAO_PLUGIN_ACCESSIBLE_UI_VERSION_2, ui)
            && Query(host, TAO_PLUGIN_WIDGETS_INTERFACE, TAO_PLUGIN_WIDGETS_VERSION_1, widgets)
            && Query(host, TAO_PLUGIN_BOARD_ACTIONS_INTERFACE, TAO_PLUGIN_BOARD_ACTIONS_VERSION_2, board)
            && Query(host, TAO_PLUGIN_SIMULATION_INTERFACE, TAO_PLUGIN_SIMULATION_VERSION_1, simulation)
            && Query(host, TAO_PLUGIN_AUDIO_GROUP_INTERFACE, TAO_PLUGIN_AUDIO_GROUP_VERSION_1, audio)
            && Query(host, TAO_PLUGIN_INPUT_EVENTS_INTERFACE, TAO_PLUGIN_INPUT_EVENTS_VERSION_2, input)
            && Query(host, TAO_PLUGIN_VIEWS_INTERFACE, TAO_PLUGIN_VIEWS_VERSION_1, views)))
            return false;
        Query(host, TAO_PLUGIN_GAMEPLAY_INTERFACE, TAO_PLUGIN_GAMEPLAY_VERSION_1, gameplay);
        Query(host, TAO_PLUGIN_METHODS_INTERFACE, TAO_PLUGIN_METHODS_VERSION_2, methods);
        Query(host, TAO_PLUGIN_AUDIO_INTERFACE, TAO_PLUGIN_AUDIO_VERSION_1, sound);
        Query(host, TAO_PLUGIN_NAVIGATION_INTERFACE, TAO_PLUGIN_NAVIGATION_VERSION_1, navigation);
        return true;
    }

private:
    template<class T>
    static bool Query(const TaoPluginHostV2* host, const char* name, uint32_t version, T& api) {
        return host->query_interface(host->host_context, tao::AbiString(name), version, &api, sizeof(api)) == TAO_PLUGIN_OK;
    }
};

// ABI 字符串是借用视图，收到后必须立即复制。
inline std::string CopyView(TaoPluginStringView view) { return {view.data ? view.data : "", view.size}; }
inline bool SameHandle(TaoPluginObjectHandle a, TaoPluginObjectHandle b) {
    return a.object_id == b.object_id && a.generation == b.generation && a.world_generation == b.world_generation;
}
inline bool SameNode(TaoPluginAccessibleNodeV2 a, TaoPluginAccessibleNodeV2 b) {
    return SameHandle(a.widget, b.widget) && a.item == b.item;
}

// 阻塞型 ui.invoke(ACTIVATE) 的在栈标志：某些按钮（如未解锁模式按钮）的激活会
// 打开消息框模态循环，invoke 直到模态关闭才返回；期间宿主嵌套泵 OnUpdate，若
// 输入回调在此期间再排队/分发激活动作，会与栈上的 invoke 重叠导致宿主拆帧崩溃
//（2026-10-09 生存模式未解锁对话框 trace 实证）。置位期间按键全部放行原生
//（模态循环有游戏自己的键盘处理：回车确认/ESC 取消），模组侧激活与原生激活
// 永不重叠。
namespace invokeguard {
inline std::atomic<int> depth{0};
struct Guard {
    Guard() { depth.fetch_add(1, std::memory_order_release); }
    ~Guard() { depth.fetch_sub(1, std::memory_order_release); }
};
// 是否有阻塞型激活仍在栈上（可嵌套：模态关闭的嵌套激活会叠加计数）。
inline bool blocked() { return depth.load(std::memory_order_acquire) > 0; }
} // namespace invokeguard
