// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "LoggingOptions.hpp"
#include "InputMap.hpp"
#include <tao/Tao.hpp>
#include <functional>
#include <optional>
#include <vector>

// 宿主 Mod 管理器只提供启用和排序，配置 schema 由 Mod 自己呈现。
class LoggingPanel {
public:
    void Connect(tao::UiController& controller, LoggingOptions& options,
        std::function<void(std::string)> say, std::function<void()> closed,
        std::function<bool()> getWaveCountdown = nullptr,
        std::function<bool(bool)> setWaveCountdown = nullptr) {
        ui = &controller; config = &options; speak = std::move(say); onClosed = std::move(closed);
        getCountdown = std::move(getWaveCountdown); setCountdown = std::move(setWaveCountdown);
    }
    bool Active() const { return open && window.IsOpen(); }
    void RequestOpen() { opening = true; }
    void Queue(Action action, int direction = 1) {
        if (actions.size() < 8) actions.push_back({action, direction});
    }
    void Pump() {
        if (opening) { opening = false; Show(); }
        if (open && !window.IsOpen()) { FinishClose(); return; }
        if (!Active()) { actions.clear(); return; }
        if (closeRequested) { closeRequested = false; Close(); return; }
        // 原生鼠标复选框同时可能产生 click/change；显式 change 值优先。
        for (int i = 0; i < kNumCheckboxes; ++i) if (changes[i]) {
            const bool value = *changes[i]; changes[i].reset(); Apply(i, value);
        }
        auto pending = std::move(actions); actions.clear();
        for (const auto& action : pending) {
            if (!Active()) break;
            switch (action.first) {
            case Action::Option: Move(action.second); break;
            case Action::Up: case Action::Left: Move(-1); break;
            case Action::Down: case Action::Right: Move(1); break;
            case Action::Confirm:
                if (index == kNumCheckboxes) Close(); else Apply(index, !Checked(index));
                break;
            case Action::Deny: case Action::Start: case Action::Settings: Close(); break;
            case Action::Info1: Announce(); break;
            default: break;
            }
        }
    }
private:
    static constexpr int kNumItems = 4;
    static constexpr int kNumCheckboxes = 3;
    static constexpr const char* keys[] = {"trace_log", "mod_log", "wave_countdown", "close"};
    static constexpr const char* labels[] = {"启用 Trace 日志", "启用 Mod 日志", "波次刷新倒计时提示", "关闭设置"};
    bool Checked(int i) const {
        if (i == 0) return tracefile::Enabled();
        if (i == 1) return config ? config->ModEnabled() : true;
        if (i == 2) return getCountdown ? getCountdown() : waveCountdownFallback;
        return false;
    }
    void Show() {
        if (Active()) { Close(); return; }
        if (!ui || !ui->Available()) { speak("Mod 设置界面不可用"); return; }
        if (config) config->Refresh();
        using namespace tao::ui;
        window = ui->Show(Panel("pvz-a11y-settings", {150,110,500,360}, {
            Text("title", "PvZ 无障碍辅助设置", {20,15,460,35}),
            Text("instructions", "方向键或 Tab 切换，回车切换开关，Esc 关闭。", {20,55,460,45}),
            Checkbox(keys[0], labels[0], Checked(0), {20,108,460,38}),
            Checkbox(keys[1], labels[1], Checked(1), {20,154,460,38}),
            Checkbox(keys[2], labels[2], Checked(2), {20,200,460,38}),
            Text("note", "修改立即保存。关闭日志不影响朗读。", {20,250,460,30}),
            Button(keys[3], labels[3], {20,296,460,42})
        }), {.modal = true, .sceneScoped = false});
        for (int i = 0; i < kNumCheckboxes; ++i) {
            window.OnClick(keys[i], [this, i] { if (!changes[i]) changes[i] = !Checked(i); });
            window.OnChange(keys[i], [this, i](std::string text) {
                changes[i] = !text.empty() && text[0] == '1';
            });
        }
        window.OnClick(keys[3], [this] { closeRequested = true; });
        open = true; index = 0;
        window.Focus(keys[index]);
        speak("PvZ 无障碍辅助设置。方向键或 Tab 切换，回车切换开关，Esc 关闭。" + State(index));
    }
    std::string State(int i) const {
        return std::string(labels[i]) + (i < kNumCheckboxes ? (Checked(i) ? "，复选框，已选中" : "，复选框，未选中") : "");
    }
    void Announce() { speak(State(index)); }
    void Move(int direction) { index = (index + direction + kNumItems) % kNumItems; window.Focus(keys[index]); Announce(); }
    void Apply(int i, bool enabled) {
        if (i < 2) {
            if (!config || !config->Set(keys[i], enabled)) speak("日志设置保存失败，请重试");
        } else if (i == 2) {
            waveCountdownFallback = enabled;
            if (setCountdown && !setCountdown(enabled)) speak("倒计时设置保存失败，请重试");
        }
        window.SetText(keys[i], tao::ui::CheckboxText(labels[i], Checked(i)));
        speak(State(i));
    }
    void Close() { (void)window.Close(); FinishClose(); }
    void FinishClose() {
        open = false; closeRequested = false; actions.clear();
        for (int i = 0; i < kNumCheckboxes; ++i) changes[i].reset();
        speak("已关闭无障碍设置"); if (onClosed) onClosed();
    }
    tao::UiController* ui = nullptr;
    LoggingOptions* config = nullptr;
    tao::UiWindow window;
    bool open = false, opening = false, closeRequested = false;
    int index = 0;
    std::optional<bool> changes[kNumCheckboxes];
    std::vector<std::pair<Action, int>> actions;
    std::function<void(std::string)> speak;
    std::function<void()> onClosed;
    std::function<bool()> getCountdown;
    std::function<bool(bool)> setCountdown;
    bool waveCountdownFallback = true;
};
