// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "LoggingOptions.hpp"
#include "InputMap.hpp"
#include <tao/Tao.hpp>
#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

// 宿主 Mod 管理器只提供启用和排序，配置 schema 由 Mod 自己呈现。
class LoggingPanel {
public:
    enum class VolumeKind { Hammer, WaveCountdown, ZombieEntry, RowBrowse };
    void ConnectRowBrowse(std::function<int()> getMode, std::function<bool(int)> setMode,
        std::function<bool()> getSound, std::function<bool(bool)> setSound) {
        getRowMode = std::move(getMode); setRowMode = std::move(setMode);
        getRowSound = std::move(getSound); setRowSound = std::move(setSound);
    }
    void Connect(tao::UiController& controller, LoggingOptions& options,
        std::function<void(std::string)> say, std::function<void()> closed,
        std::function<bool()> getWaveCountdown = nullptr,
        std::function<bool(bool)> setWaveCountdown = nullptr,
        std::function<int(VolumeKind)> getCueVolume = nullptr,
        std::function<bool(VolumeKind, int)> setCueVolume = nullptr,
        std::function<void(VolumeKind)> previewCue = nullptr) {
        ui = &controller; config = &options; speak = std::move(say); onClosed = std::move(closed);
        getCountdown = std::move(getWaveCountdown); setCountdown = std::move(setWaveCountdown);
        getVolume = std::move(getCueVolume); setVolume = std::move(setCueVolume); preview = std::move(previewCue);
    }
    bool Active() const { return open && window.IsOpen(); }
    void RequestOpen() { opening = true; }
    void Queue(Action action, int direction = 1) {
        if (actions.size() < 8) actions.push_back({action, direction});
    }
    void Pump() {
        if (opening) { opening = false; Show(); }
        if (open && !window.IsOpen()) { FinishClose(); return; }
        if (!Active()) { actions.clear(); volumeActions.clear(); return; }
        if (closeRequested) { closeRequested = false; Close(); return; }
        // 原生鼠标复选框同时可能产生 click/change；显式 change 值优先。
        for (int i : {0, 1, 2, kRowSound}) if (changes[i]) {
            const bool value = *changes[i]; changes[i].reset(); Apply(i, value);
        }
        if (rowModeClickPending) { rowModeClickPending = false; index = kRowMode; window.Focus(keys[index]); CycleRowMode(1); }
        auto mouseVolumes = std::move(volumeActions); volumeActions.clear();
        for (const auto& [item, delta] : mouseVolumes) {
            index = item; window.Focus(keys[index]); AdjustVolume(delta);
        }
        auto pending = std::move(actions); actions.clear();
        for (const auto& action : pending) {
            if (!Active()) break;
            switch (action.first) {
            case Action::Option: Move(action.second); break;
            case Action::Up: Move(-1); break;
            case Action::Down: Move(1); break;
            case Action::Left: if (IsVolume(index)) AdjustVolume(-5); else if (index == kRowMode) CycleRowMode(-1); else Move(-1); break;
            case Action::Right: if (IsVolume(index)) AdjustVolume(5); else if (index == kRowMode) CycleRowMode(1); else Move(1); break;
            case Action::Confirm:
                if (index == kClose) Close();
                else if (IsVolume(index)) AdjustVolume(0);
                else if (index == kRowMode) CycleRowMode(1);
                else Apply(index, !Checked(index));
                break;
            case Action::Deny: case Action::Start: case Action::Settings: Close(); break;
            case Action::Info1: Announce(); break;
            default: break;
            }
        }
    }
private:
    static constexpr int kNumItems = 10;
    static constexpr int kNumCheckboxes = 3;
    static constexpr int kRowMode = 6, kRowSound = 7, kRowVolume = 8, kClose = 9;
    static constexpr const char* keys[] = {"trace_log", "mod_log", "wave_countdown", "hammer_volume", "wave_countdown_volume", "zombie_entry_volume", "row_zombie_speech", "row_zombie_cue", "row_zombie_volume", "close"};
    static constexpr const char* labels[] = {"启用 Trace 日志", "启用 Mod 日志", "波次刷新倒计时提示", "巨人举锤音量", "僵尸刷新倒计时音量", "僵尸入场音量", "跨行僵尸朗读", "跨行僵尸三角波音效", "跨行僵尸音效音量", "关闭设置"};
    static bool IsVolume(int i) { return (i >= kNumCheckboxes && i < kRowMode) || i == kRowVolume; }
    static int VolumeSlot(int i) { return i == kRowVolume ? 3 : i - kNumCheckboxes; }
    int RowMode() const { return std::clamp(getRowMode ? getRowMode() : rowModeFallback, 0, 2); }
    std::string RowModeText() const {
        static const char* modes[] = {"关闭", "只朗读数量", "朗读数量和坐标"};
        return std::string(labels[kRowMode]) + "：" + modes[RowMode()];
    }
    void CycleRowMode(int delta) {
        rowModeFallback = (RowMode() + delta + 3) % 3;
        if (setRowMode && !setRowMode(rowModeFallback)) speak("朗读设置未能保存，仅本次会话有效");
        window.SetText(keys[kRowMode], RowModeText()); speak(State(kRowMode));
    }
    int Volume(int i) const {
        const auto kind = static_cast<VolumeKind>(VolumeSlot(i));
        return std::clamp(getVolume ? getVolume(kind) : volumeFallback[VolumeSlot(i)], 0, 100);
    }
    std::string VolumeText(int i) const { return std::string(labels[i]) + "：" + std::to_string(Volume(i)) + "%"; }
    tao::ui::Node VolumeRow(int i, int y) {
        using namespace tao::ui;
        return Panel(std::string(keys[i]) + "_row", {20,y,460,38}, {
            Button(std::string(keys[i]) + "_minus", "减小", {0,0,60,38}),
            Button(keys[i], VolumeText(i), {65,0,330,38}),
            Button(std::string(keys[i]) + "_plus", "增大", {400,0,60,38})
        });
    }
    bool Checked(int i) const {
        if (i == 0) return tracefile::Enabled();
        if (i == 1) return config ? config->ModEnabled() : true;
        if (i == 2) return getCountdown ? getCountdown() : waveCountdownFallback;
        if (i == kRowSound) return getRowSound ? getRowSound() : rowSoundFallback;
        return false;
    }
    void Show() {
        if (Active()) { Close(); return; }
        if (!ui || !ui->Available()) { speak("Mod 设置界面不可用"); return; }
        if (config) config->Refresh();
        using namespace tao::ui;
        window = ui->Show(Panel("pvz-a11y-settings", {150,15,500,570}, {
            Text("title", "PvZ 无障碍辅助设置", {20,15,460,35}),
            Text("instructions", "上下或 Tab 切换，左右调整，回车试听或切换。", {20,50,460,35}),
            Checkbox(keys[0], labels[0], Checked(0), {20,95,460,32}),
            Checkbox(keys[1], labels[1], Checked(1), {20,133,460,32}),
            Checkbox(keys[2], labels[2], Checked(2), {20,171,460,32}),
            VolumeRow(3, 209), VolumeRow(4, 247), VolumeRow(5, 285),
            Button(keys[kRowMode], RowModeText(), {20,323,460,38}),
            Checkbox(keys[kRowSound], labels[kRowSound], Checked(kRowSound), {20,361,460,38}),
            VolumeRow(kRowVolume, 399),
            Text("note", "音量每次调整 5%，修改后立即试听和保存。", {20,445,460,30}),
            Button(keys[kClose], labels[kClose], {20,500,460,42})
        }), {.modal = true, .sceneScoped = false});
        for (int i : {0, 1, 2, kRowSound}) {
            window.OnClick(keys[i], [this, i] { if (!changes[i]) changes[i] = !Checked(i); });
            window.OnChange(keys[i], [this, i](std::string text) {
                changes[i] = !text.empty() && text[0] == '1';
            });
        }
        window.OnClick(keys[kRowMode], [this] { rowModeClickPending = true; });
        for (int i : {3, 4, 5, kRowVolume}) {
            const auto queue = [this, i](int delta) { if (volumeActions.size() < 8) volumeActions.push_back({i, delta}); };
            window.OnClick(keys[i], [queue] { queue(0); });
            window.OnClick(std::string(keys[i]) + "_minus", [queue] { queue(-5); });
            window.OnClick(std::string(keys[i]) + "_plus", [queue] { queue(5); });
        }
        window.OnClick(keys[kClose], [this] { closeRequested = true; });
        open = true; index = 0;
        window.Focus(keys[index]);
        speak("PvZ 无障碍辅助设置。上下或 Tab 切换，左右调音量，回车试听或切换开关，Esc 关闭。" + State(index));
    }
    std::string State(int i) const {
        if (i == kRowMode) return RowModeText() + "，左右或回车切换";
        if (IsVolume(i)) return VolumeText(i) + "，左右键调整，回车试听";
        return std::string(labels[i]) + (i < kNumCheckboxes || i == kRowSound ? (Checked(i) ? "，复选框，已选中" : "，复选框，未选中") : "");
    }
    void Announce() { speak(State(index)); }
    void AdjustVolume(int delta) {
        const auto kind = static_cast<VolumeKind>(VolumeSlot(index));
        const int old = Volume(index), next = std::clamp(old + delta, 0, 100);
        if (next != old) {
            volumeFallback[VolumeSlot(index)] = next;
            if (setVolume && !setVolume(kind, next)) speak("音量已应用，保存失败，仅本次会话有效");
            window.SetText(keys[index], VolumeText(index));
        }
        speak(State(index));
        if (preview && (next != old || delta == 0)) preview(kind);
    }
    void Move(int direction) { index = (index + direction + kNumItems) % kNumItems; window.Focus(keys[index]); Announce(); }
    void Apply(int i, bool enabled) {
        if (i < 2) {
            if (!config || !config->Set(keys[i], enabled)) speak("日志设置保存失败，请重试");
        } else if (i == 2) {
            waveCountdownFallback = enabled;
            if (setCountdown && !setCountdown(enabled)) speak("倒计时设置保存失败，请重试");
        } else if (i == kRowSound) {
            rowSoundFallback = enabled;
            if (setRowSound && !setRowSound(enabled)) speak("音效设置未能保存，仅本次会话有效");
        }
        window.SetText(keys[i], tao::ui::CheckboxText(labels[i], Checked(i)));
        speak(State(i));
    }
    void Close() { (void)window.Close(); FinishClose(); }
    void FinishClose() {
        // 原生关闭同样释放 SDK 文档和回调，防止重新打开后旧窗口继续接收事件。
        (void)window.Close();
        open = false; closeRequested = false; actions.clear(); volumeActions.clear();
        for (auto& change : changes) change.reset();
        rowModeClickPending = false;
        speak("已关闭无障碍设置"); if (onClosed) onClosed();
    }
    tao::UiController* ui = nullptr;
    LoggingOptions* config = nullptr;
    tao::UiWindow window;
    bool open = false, opening = false, closeRequested = false;
    int index = 0;
    std::optional<bool> changes[kNumItems];
    std::vector<std::pair<Action, int>> actions;
    std::vector<std::pair<int, int>> volumeActions;
    std::function<void(std::string)> speak;
    std::function<void()> onClosed;
    std::function<bool()> getCountdown;
    std::function<bool(bool)> setCountdown;
    bool waveCountdownFallback = true;
    int volumeFallback[4] = {100, 100, 100, 100};
    int rowModeFallback = 1;
    bool rowSoundFallback = true, rowModeClickPending = false;
    std::function<int()> getRowMode;
    std::function<bool(int)> setRowMode;
    std::function<bool()> getRowSound;
    std::function<bool(bool)> setRowSound;
    std::function<int(VolumeKind)> getVolume;
    std::function<bool(VolumeKind, int)> setVolume;
    std::function<void(VolumeKind)> preview;
};
