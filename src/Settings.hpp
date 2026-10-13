// SPDX-License-Identifier: LGPL-3.0-or-later
// 设置持久化：写入当前玩家档案。未选择用户时仅本次会话有效。
// 对应 PvZA11y 的 Config.cs；后续选项（提示音音量组、键位绑定等）在此扩展字段。
#pragma once
#include <functional>

struct SettingsData {
    bool enabled = true;
    int volume = 80;
    int rate = 0;
    double cues = 1.0;
    bool zombieEntryCue = true;  // 每波僵尸入场按行递降音高提示
    bool fastZombieCue = true;   // 撑杆/橄榄球/雪橇小队出现三连音提示
    bool gargantuarHammerCue = true; // 巨人/红眼巨人每次举锤时按行播放双声锯齿音
    int gargantuarHammerVolume = 100; // 各类提示音独立音量（0–100%，叠加 cues 总增益）
    int waveCountdownVolume = 100;
    int zombieEntryVolume = 100;
    int rowZombieSpeechMode = 1; // 跨行朗读：0=关闭，1=数量，2=数量和坐标（同 Shift）
    bool rowZombieCue = true;
    int rowZombieVolume = 100;
    bool plantReadyCue = true;   // 当前选中卡片冷却完成且阳光充足时琶音提示
    bool waveCountdownCue = true; // 下一波僵尸刷新倒计时方波提示
    int waveCountdownSeconds = 5; // 刷新前提醒倒计时秒数（默认 5 秒）
    int storeX = 0;              // 主菜单商店入口按钮位置（动态学习，w>0 时有效）
    int storeY = 0;
    int storeW = 0;
    int storeH = 0;
    int gridRowsAdv1 = 0;        // 冒险 1-1 可玩行数（选卡阶段按小推车权威探测并持久化，
    int gridRowsAdv2 = 0;        // 1-2 同理。首周目 1/3 行、二周目满 5 行，与关卡号无关；
    int gridRowsAdv3 = 0;        // 1-3 同理。0=未知，供续玩存档（小推车可能已被消耗）回读）
};

class Settings {
public:
    using LoadHook = std::function<bool(SettingsData&)>;
    using SaveHook = std::function<bool(const SettingsData&)>;

    void SetHooks(LoadHook load, SaveHook save) {
        loadHook = std::move(load);
        saveHook = std::move(save);
    }

    SettingsData& Data() { return data; }
    bool Loaded() const { return loaded; }

    void Load() {
        SettingsData next = data;
        if (loadHook && loadHook(next)) {
            data = next;
            loaded = true;
        }
    }
    bool Save() { return saveHook && saveHook(data); }

private:
    LoadHook loadHook;
    SaveHook saveHook;
    SettingsData data;
    bool loaded = false;
};
