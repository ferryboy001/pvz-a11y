// SPDX-License-Identifier: LGPL-3.0-or-later
// 棋盘语义控制：格子光标、种子包、收集、铲子、冻结与单步。
// 关卡末尾奖励（通关种子包/奖杯/铲子等）由 AutoCollectRewards 自动领取。
// 后续迁移落点：僵尸声纳/绊线播报、植物就绪与位置提示音。
#pragma once
#include "HostServices.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#endif

// Windows 的 GetMessage 是对象式宏（#define GetMessage GetMessageA），会破坏
// Board::GetMessage 成员调用；本地取消该宏。
#ifdef GetMessage
#undef GetMessage
#endif

// 宿主导出（Engine.hpp 的 TaoEngineResolveObject）：把语义对象句柄解成真实对象指针。
// 经 runtime/TaoEngineImport.cpp 的延迟加载钩子重定向到游戏 exe（与 TodStringTranslate 同机制）。
// 仅用于读取宿主语义接口未暴露的字段：Board::mZombieAllowed。
extern "C" void* TaoEngineResolveObject(TaoPluginObjectHandle handle) noexcept;

// 20261005 版 exe：Board::mZombieAllowed（bool[100]，按 ZombieType 索引）在 Board 对象内的
// 字节偏移。由反汇编 Challenge::InitZombieWavesFromList 确认：该函数即
// `for t in list: board->mZombieAllowed[t] = 1`，写址为 [board + 0x5e30 + t]。
static constexpr size_t kZombieAllowedOffset = 0x5e30;

// 僵王博士火球/冰球：球不是投射物，而是挂在博士僵尸（ZOMBIE_BOSS）身上的
// reanimation（REANIM_BOSS_FIREBALL/ICEBALL，id 存于 Zombie::mBossFireBallReanimID，
// 行号在 mFireballRow、冰火在 mIsFireball）。列坐标宿主语义接口没有暴露，需经
// LawnApp::ReanimationTryToGet 解出 reanimation 指针后读 mOverlayMatrix 平移分量
//（SetPosition 写 m02/m12；宿主 SceneAnimationChecks.inl 也按此布局读取）。
// 按 SDK Reanimator.h 的 Reanimation 字段布局（x64）：
//   4B type + 2x4B anim + 对齐 + 8B definition + 4B loop + 1B dead + 对齐
//   + 3x4B frame 字段 → mOverlayMatrix 在 +0x2C；translateX(m02)=+0x34、
//   translateY(m12)=+0x40。换 exe 版本需按 Reanimator.h 布局重新确认。
static constexpr size_t kReanimPosXOffset = 0x34;
static constexpr size_t kReanimPosYOffset = 0x40;

// 禅境花园工具表：顺序与宿主无障碍工具条一致（ModAccessibilityUi.inl 的 tools[]），
// 数字键 1..8 固定映射同一顺序。object/cursor 为 GameObjectType / CursorType 裸值：
// 浇水壶 6→9、肥料 7→10、杀虫剂 8→11、留声机 9→12、巧克力 10→13、
// 手套 11→14、卖植物 12→15、手推车 13→16。
struct ZenToolDef { int object; int cursor; const char* name; };
static const ZenToolDef kZenTools[] = {
    {6, 9, "浇水壶"},   // OBJECT_TYPE_WATERING_CAN  → CURSOR_TYPE_WATERING_CAN
    {7, 10, "肥料"},    // OBJECT_TYPE_FERTILIZER    → CURSOR_TYPE_FERTILIZER
    {8, 11, "杀虫剂"},  // OBJECT_TYPE_BUG_SPRAY     → CURSOR_TYPE_BUG_SPRAY
    {9, 12, "留声机"},  // OBJECT_TYPE_PHONOGRAPH    → CURSOR_TYPE_PHONOGRAPH
    {10, 13, "巧克力"}, // OBJECT_TYPE_CHOCOLATE     → CURSOR_TYPE_CHOCOLATE
    {11, 14, "手套"},   // OBJECT_TYPE_GLOVE         → CURSOR_TYPE_GLOVE
    {12, 15, "卖植物"}, // OBJECT_TYPE_MONEY_SIGN    → CURSOR_TYPE_MONEY_SIGN
    {13, 16, "手推车"}, // OBJECT_TYPE_WHEELBARROW   → CURSOR_TYPE_WHEEELBARROW
};

// 宿主导出 Challenge::IZombieSeedTypeToZombieType(SeedType)→ZombieType（静态成员，
// 游戏自己的僵尸包类型映射，见 lib/TaoEngineHost.def）。静态成员函数在 x64 下
// ABI 与自由函数一致，经 GetProcAddress 探测 + 函数指针调用（同 TodStringTranslate
// 的探测模式），缺失时调用方走回退命名。
using IzombieSeedTypeToZombieTypeFn = int (*)(int);
static const char kIzombieSeedTypeToZombieType[] =
    "?IZombieSeedTypeToZombieType@Challenge@@SA?AW4ZombieType@@W4SeedType@@@Z";

// 宿主导出 TodStringTranslate（链接 TaoEngineHost.lib，延迟加载重定向到游戏 exe，
// 同 ChallengeNames/Award）。调用前必须确认导出存在（kTodStringTranslateExport
// 探测），否则延迟加载失败会抛异常。
std::string __cdecl TodStringTranslate(std::string_view theString);
static const char kTodStringTranslateExport[] =
    "?TodStringTranslate@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$basic_string_view@DU?$char_traits@D@std@@@2@@Z";

// 宿主导出成员函数（见 lib/TaoEngineHost.def，链接 TaoEngineHost.lib 延迟加载重定向）。
// x64 下成员调用 ABI 与"首参为 this 的 __cdecl 自由函数"一致（rcx=this，edx=首参），
// 用 /alternatename 把本地别名映射到导出修饰名：
//   - LawnApp::CanPauseNow() —— Space 暂停走的同一前置门禁（语义 ABI 的 "can_pause"）
//   - LawnApp::DoNewOptions(bool) —— 打开选项（暂停）对话框，棋盘内 ESC 的原生终点
//   - TaoEngineGetApp() —— 当前 LawnApp*（Engine.hpp 同款声明）
//   - LawnApp::ReanimationTryToGet(ReanimationID) —— 由 reanim id 取 Reanimation*
//     （僵王球列定位；TaoEngineResolveObject 对手工构造的 reanimation 句柄
//     代际校验不过会返回空，实测）
#if defined(_MSC_VER)
extern "C" bool __cdecl LawnApp_CanPauseNow(const void* self) noexcept;
extern "C" void __cdecl LawnApp_DoNewOptions(void* self, bool theFromGameSelector) noexcept;
extern "C" void* __cdecl LawnApp_ReanimationTryToGet(void* self, uint32_t theId) noexcept;
#pragma comment(linker, "/alternatename:LawnApp_CanPauseNow=?CanPauseNow@LawnApp@@QEAA_NXZ")
#pragma comment(linker, "/alternatename:LawnApp_DoNewOptions=?DoNewOptions@LawnApp@@QEAAX_N@Z")
#pragma comment(linker, "/alternatename:LawnApp_ReanimationTryToGet=?ReanimationTryToGet@LawnApp@@QEAAPEAVReanimation@@W4ReanimationID@@@Z")
#else
extern "C" {
    bool _ZN7LawnApp11CanPauseNowEv(const void* self) noexcept;
    void _ZN7LawnApp12DoNewOptionsEb(void* self, bool theFromGameSelector) noexcept;
    void* _ZN7LawnApp19ReanimationTryToGetE13ReanimationID(void* self, uint32_t theId) noexcept;
}
inline bool LawnApp_CanPauseNow(const void* self) noexcept { return _ZN7LawnApp11CanPauseNowEv(self); }
inline void LawnApp_DoNewOptions(void* self, bool theFromGameSelector) noexcept { _ZN7LawnApp12DoNewOptionsEb(self, theFromGameSelector); }
inline void* LawnApp_ReanimationTryToGet(void* self, uint32_t theId) noexcept { return _ZN7LawnApp19ReanimationTryToGetE13ReanimationID(self, theId); }
#endif
extern "C" void* TaoEngineGetApp() noexcept;

class BoardControl {
public:
    explicit BoardControl(const HostServices* host) : host(host) {}

    void SetSay(std::function<void(std::string, bool)> sink) { say = std::move(sink); }
    void SetObjects(const tao::Objects* value) { objects = value; }
    void SetLog(std::function<void(std::string)> sink) { log = std::move(sink); }

    TaoPluginAccessibilityContextV1 Context() {
        TaoPluginAccessibilityContextV1 context{};
        context.struct_size = sizeof(context);
        if (host->a11y.context(host->a11y.host_context, &context) != TAO_PLUGIN_OK) return {};
        if (!SameHandle(currentBoard, context.board)) {
            currentBoard = context.board;
            column = row = 0;
            layer = 0;
            seedIndex = 0;
            boardFocus = false;
            cycleZombieKey = 0;
            diagLogged = false;
            bossBallResolveLogged = false;
            zenProbeValid = false;
            gridProbeValid = false;
            gardenCarried = {};
        }
        // 禅境花园实际网格 8x4（ZEN_MAX_GRIDSIZE_X/Y，宿主花园落点枚举的同款
        // 范围），宿主 context 按草坪棋盘上报 9x6；覆写后光标夹紧、格子朗读、
        // 花园总览统计都落在真实范围内。普通关卡同理按场地/关卡收窄（ProbeGrid）。
        int rowMin = 0;
        if (IsZenGarden()) {
            context.columns = 8;
            context.rows = 4;
        } else {
            ProbeGrid(context);
            context.columns = gridColumns;
            context.rows = gridRows;
            rowMin = gridRowMin;
        }
        column = std::clamp(column, 0, std::max(0, context.columns - 1));
        row = std::clamp(row, rowMin, std::max(rowMin, rowMin + context.rows - 1));
        if (currentBoard.object_id) LogModeDiagnostics(context);
        return context;
    }
    bool HasBoardFocus() const { return boardFocus; }
    void SetBoardFocus(bool value) { boardFocus = value; }

    // 输入落点判定：正常战斗（scene=3），或 1-5 戴夫对话后的"挖植物"教学
    // （scene=2 且铲子教学激活）。后者宿主场景仍停在选卡与关卡准备，但棋盘
    // 已可交互；宿主对 TAO_BOARD_SHOVEL / SELECT_SHOVEL / CANCEL 也按教学
    // 状态放行（宿主 BoardActionGate 的 shovelTutorial 分支），所以此阶段
    // 必须把输入落到棋盘而不是控件导航，否则无法选铲、挖植物进入保龄球。
    bool Gameplay(const TaoPluginAccessibilityContextV1& context) {
        if (!context.board.object_id || (context.flags & TAO_INPUT_MODAL)) return false;
        return context.scene == 3 || (context.scene == 2 && InShovelTutorial());
    }
    bool InShovelTutorial() {
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const auto state = boardResult.Value()->GetTutorialState();
        if (!state) return false;
        const int value = int(state.Value().Value());
        return value >= int(tao::enums::TutorialState::TUTORIAL_SHOVEL_PICKUP.Value()) &&
            value <= int(tao::enums::TutorialState::TUTORIAL_SHOVEL_KEEP_DIGGING.Value());
    }

    // 草坪实际网格探针（随棋盘缓存）：宿主统一按 MAX_GRID_SIZE 上报 9x6，
    // 场地行数因背景而异——泳池/浓雾（BACKGROUND_3_POOL=2 / 4_FOG=3）6 行，
    // 前院/夜晚/屋顶 5 行。存在"部分可玩行"的只有冒险 1-1..1-3 首周目
    //（1-1 只有中间 1 行、1-2/1-3 中间 3 行；二周目即满 5 行），关卡号无法
    // 区分周目，改按小推车分布推断（每个可走行一辆，泳池中行为泳池清洁车）。
    // 可信度分级：①scene==2 选卡阶段小推车必然完整在场，行分布权威，探测
    // 结果按关卡写入档案（grid_rows_adv1/2/3，SetGridMemory）——续玩存档
    // 直接回 scene==3，边缘小推车可能已被消耗，跨度会假性偏小，战斗中首探
    // 针绝不能自行收窄；②战斗中看到满编小推车可直接认定满行并自愈档案；
    // ③其余情况（含我是僵尸/僵王等本无小推车的关卡，但它们不在冒险 1..3
    // 的收窄名单里）读档案，档案未知才回退关卡号启发式。行号保持游戏绝对
    // 编号（gridRowMin 偏移），与僵尸行号/格子朗读一致。不毛之地
    //（GAMEMODE_CHALLENGE_RESODDED=38）只有中间 3 行可种，但僵尸走满 5 行，
    // 且非冒险 1..3 不参与收窄。
    void ProbeGrid(const TaoPluginAccessibilityContextV1& context) {
        if (gridProbeValid) return;
        gridProbeValid = true;
        gridColumns = context.columns;
        gridRows = context.rows;
        gridRowMin = 0;
        if (!objects || !objects->Available() || !currentBoard.object_id) return;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return;
        const tao::Board board = boardResult.Value();
        if (const auto background = board->GetBackground()) {
            const int bg = int(background.Value().Value());
            gridRows = (bg == 2 || bg == 3) ? 6 : 5;
        }
        // 僵尸提示音只烘了 H5/H6 两套资源（resources.xml），按场地高度选套，
        // 不能用收窄后的可导航行数（冒险 1-1/1-2/1-3 行号居中绝对编号，
        // 正好对应 H5 的居中行资源）。
        gridFieldRows = gridRows;
        if (CurrentGameMode() != int(tao::enums::GameMode::GAMEMODE_ADVENTURE.Value())) return;
        const auto level = board->GetLevel();
        if (!level || level.Value() < 1 || level.Value() > 3) return;
        const int adventureLevel = level.Value();
        const MowerRows mowers = ProbeMowerRows();
        if (mowers.count > 0 && context.scene == 2) {
            gridRows = std::clamp(mowers.maxRow - mowers.minRow + 1, 1, gridFieldRows);
            gridRowMin = std::clamp(mowers.minRow, 0, gridFieldRows - gridRows);
            RememberGridRows(adventureLevel, gridRows);
            return;
        }
        if (mowers.count >= gridFieldRows) { RememberGridRows(adventureLevel, gridFieldRows); return; }
        int rows = RememberedGridRows(adventureLevel);
        if (rows <= 0 || rows > gridFieldRows) rows = adventureLevel == 1 ? 1 : 3;
        if (rows < gridFieldRows) {
            gridRows = rows;
            gridRowMin = (gridFieldRows - rows) / 2;
        }
    }

    struct MowerRows { int count = 0, minRow = 0, maxRow = 0; };
    // 枚举 LawnMower 视图中未死亡小推车的行分布；无小推车（我是僵尸/僵王
    // 等）或视图不可用返回 count=0。注意续玩存档的战斗中探针可能漏掉已被
    // 消耗的边缘车，调用方按场景判断可信度（见 ProbeGrid）。
    MowerRows ProbeMowerRows() {
        MowerRows result;
        std::string view;
        if (!ViewName('m', view)) return result;
        bool any = false;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto mower = objects->Wrap<tao::LawnMower>(handle);
            if (!mower) return;
            const auto m = mower.Value();
            if (const auto dead = m->GetDead(); !dead || dead.Value()) return;
            const auto mowerRow = m->GetRow();
            if (!mowerRow) return;
            const int value = mowerRow.Value();
            ++result.count;
            if (!any || value < result.minRow) result.minRow = value;
            if (!any || value > result.maxRow) result.maxRow = value;
            any = true;
        });
        return result;
    }

    // 冒险 1-1..1-3 可玩行数的档案记忆（周目相关，选卡阶段权威探测后写入），
    // 由 Plugin 接线到档案设置 grid_rows_adv1/2/3；0=未知。
    void SetGridMemory(std::function<int(int)> load, std::function<void(int, int)> save) {
        loadGridRows = std::move(load);
        saveGridRows = std::move(save);
    }
    int RememberedGridRows(int level) const { return loadGridRows ? loadGridRows(level) : 0; }
    void RememberGridRows(int level, int rows) { if (saveGridRows) saveGridRows(level, rows); }

    void MoveCursor(int dx, int dy) {
        column += dx;
        row += dy;
        Context();
        layer = 0;
    }
    void ReadCell() {
        auto text = ComposeCellText();
        if (text.empty()) {
            TaoPluginStringView fallback{};
            if (host->a11y.cell_text(host->a11y.host_context, currentBoard, column, row, &fallback) == TAO_PLUGIN_OK)
                text = CopyView(fallback);
        }
        if (!text.empty()) Say(std::move(text));
        boardFocus = true;
        // 宝石迷阵/旋风：当前格有可达匹配（拖动/旋转能凑成三消）时哔一声。
        if (IsBeghouledLevel() || IsBeghouledTwistLevel()) MatchCue();
    }

    static const char* ReasonText(uint32_t reason) {
        static const char* reasons[] = {"成功", "目标已失效", "当前不能操作", "此模式不支持", "请先选择种子或工具",
            "仍在冷却", "阳光不足", "不满足使用条件", "格子无效", "目标不可操作", "操作未执行"};
        return reasons[reason ? std::min<uint32_t>(reason, 10) : 10];
    }
    bool InvokeRaw(uint32_t action, TaoPluginObjectHandle target, TaoPluginBoardActionResultV2& result) {
        TaoPluginBoardActionRequestV2 request{};
        request.struct_size = sizeof(request);
        request.board = currentBoard;
        request.action = action;
        request.target = target;
        request.column = column;
        request.row = row;
        result.struct_size = sizeof(result);
        return host->board.invoke(host->board.host_context, &request, &result) == TAO_PLUGIN_OK;
    }
    bool Invoke(uint32_t action, TaoPluginObjectHandle target = {}, bool announce = true) {
        TaoPluginBoardActionResultV2 result{};
        const bool done = InvokeRaw(action, target, result);
        if (announce) Say(done ? "操作完成" : ReasonText(result.reason));
        return done;
    }
    void SelectSeed(size_t index) {
        if (IsIZombieLevel()) { SelectZombiePacket(index); return; }
        seedIndex = int(index);
        TaoPluginBoardTargetV2 target{};
        target.struct_size = sizeof(target);
        if (host->board.target_at(host->board.host_context, currentBoard, TAO_BOARD_SELECT_SEED, index, &target) != TAO_PLUGIN_OK) {
            Say("没有这个种子包");
            return;
        }
        const auto name = CopyView(target.name);
        // 播报格式：卡片名，N阳光；异常状态的卡槽把异常放在卡片名之后。
        const std::string cost = std::to_string(target.cost) + "阳光";
        // 持铲/持卡等非空光标时的换卡流程（实机排障结论）：
        // 语义 CANCEL 对铲子只改 a11y 上报状态，真实光标里铲子并未放回，
        // 此后一切选卡（原生点击 / 语义 SELECT_SEED）都被门控拒绝（reason=5）。
        // 因此持铲时先用原生点击铲子槽把铲子真正放回（原版鼠标路径），再选卡。
        // 打僵尸关卡的常驻锤子光标等同普通关卡的空光标：无法也不必放下，直接选卡；
        // 语义选择若仍被门控拒绝，下方退到原生点击（原版持锤点卡即拾取）。
        const int cursorBefore = Context().cursor;
        const bool whackHammer =
            cursorBefore == int(tao::enums::CursorType::CURSOR_TYPE_HAMMER.Value()) && IsWhackLevel();
        if (cursorBefore != int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value()) && !whackHammer) {
            if (cursorBefore == int(tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value()))
                NativeReturnShovel();
            if (Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value()) &&
                NativeSelectSeed(target.target)) {
                boardFocus = true;
                Say(name + "，" + cost);
                return;
            }
            if (Context().cursor != int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value())) {
                TaoPluginBoardActionResultV2 cancelResult{};
                InvokeRaw(TAO_BOARD_CANCEL, {}, cancelResult);
                const int cursorAfterCancel = Context().cursor;
                if (cursorAfterCancel != int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value())) {
                    Say("请先放下当前工具");
                    return;
                }
            }
        }
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_SELECT_SEED, target.target, result) ||
            (whackHammer && NativeSelectSeed(target.target))) {
            boardFocus = true;
            Say(name + "，" + cost);
        } else {
            std::string issue = ReasonText(result.reason);
            if (result.reason == TAO_ACTION_COOLDOWN) {
                // 冷却按已恢复百分比播报（递增）：1 - 剩余冷却 / 定义刷新时间
                if (const int refresh = PlantRefreshTime(target.type); refresh > 0)
                    issue = "百分之" + std::to_string(std::clamp((refresh - target.cooldown_remaining) * 100 / refresh, 0, 100)) + " 冷却";
            }
            Say(name + "，" + issue + "，" + cost);
        }
    }
    // 种子银行可选项数（SELECT_SEED 目标个数）。
    size_t SeedBankCount() {
        size_t count = 0;
        if (currentBoard.object_id)
            host->board.target_count(host->board.host_context, currentBoard, TAO_BOARD_SELECT_SEED, &count);
        return count;
    }
    // 数字卡槽总数：种子银行 + 场上掉落卡。砸花瓶（Vasebreaker）关卡两者并存
    // （种子银行里可能有植物，场上还可能有砸罐掉落的植物卡），老虎机无种子银行。
    size_t CardSlotCount() { return SeedBankCount() + DroppedCards().size(); }
    void CycleSeed(int direction) {
        if (IsZenGarden()) { Say("禅境花园用数字键选择工具"); return; }
        if (IsSlotMachineLevel()) { Say("老虎机没有种子包，数字键拾取掉落卡"); return; }
        if (HasSpecialBank()) {
            const size_t count = BankPackets().size();
            if (!count) { Say("没有可用操作"); return; }
            SelectSpecialBankPacket(size_t((seedIndex + direction + int(count)) % int(count)));
            return;
        }
        if (IsIZombieLevel()) {
            const size_t count = BankPackets().size();
            if (!count) { Say("没有僵尸包"); return; }
            SelectZombiePacket(size_t((seedIndex + direction + int(count)) % int(count)));
            return;
        }
        const size_t count = CardSlotCount();
        if (!count) { Say("没有种子包"); return; }
        SelectCardSlot(size_t((seedIndex + direction + int(count)) % int(count)));
    }
    // 选卡界面卡片朗读素材：prefix 为名称前的限制（不推荐/水生/夜间），
    // suffix 为名称后的阳光与 tooltip（自带前导"，"）。宿主选卡（含模仿者
    // 对话框）的无障碍子项身份是固定基址 + SeedType（宿主 ModAccessibilityUi
    // 约定）：0x200000000+SeedType 为选卡卡片，0xd00000000+SeedType 为模仿者
    // 对话框卡片。返回 false 表示不是种子卡片。
    bool SeedCardInfo(uint64_t item, std::string& prefix, std::string& suffix) {
        int seedType = -1;
        if (item >= UINT64_C(0x200000000) && item < UINT64_C(0x200010000))
            seedType = int(item - UINT64_C(0x200000000));
        else if (item >= UINT64_C(0xd00000000) && item < UINT64_C(0xd00010000))
            seedType = int(item - UINT64_C(0xd00000000));
        else if (item == UINT64_C(0x10000006A)) // 选卡界面的模仿者按钮（mImitaterButton：
            seedType = int(tao::enums::SeedType::SEED_IMITATER.Value()); // GameButton mId=106，2026-10-10 DumpTree 实证）
        if (seedType < 0) return false;
        prefix.clear();
        suffix.clear();
        const bool aquatic = PlantStaticBool("Lawn.Plant.IsAquatic(Lawn.SeedType)", seedType);
        const bool nocturnal = PlantStaticBool("Lawn.Plant.IsNocturnal(Lawn.SeedType)", seedType);
        // 关卡舞台（有无水面/是否夜晚）决定是否"不推荐"：无水面选水生植物、
        // 白天选夜间植物（原版选卡 tooltip 的警告条件）。
        bool stageKnown = false, hasPool = false, isNight = false;
        if (objects && objects->Available() && currentBoard.object_id)
            if (auto boardProxy = objects->Wrap<tao::Board>(currentBoard)) {
                const auto pool = boardProxy.Value()->StageHasPool();
                const auto night = boardProxy.Value()->StageIsNight();
                if (pool && night) {
                    hasPool = pool.Value();
                    isNight = night.Value();
                    stageKnown = true;
                }
            }
        if (stageKnown && ((aquatic && !hasPool) || (nocturnal && !isNight)))
            prefix = "不推荐";
        if (aquatic) { if (!prefix.empty()) prefix += "，"; prefix += "水生植物"; }
        if (nocturnal) { if (!prefix.empty()) prefix += "，"; prefix += "夜间植物"; }
        if (const int cost = SeedCost(seedType); cost >= 0)
            suffix = "，" + std::to_string(cost) + "阳光";
        const std::string tip = PlantToolTip(seedType);
        if (!tip.empty()) suffix += "，" + tip;
        return true;
    }
    void AnnounceStatus() {
        if (IsTreeOfWisdom()) { AnnounceTreeStatus(); return; }
        const auto context = Context();
        Say("阳光 " + std::to_string(context.sun) + "，金币 " + std::to_string(context.money));
    }
    int GetWaveCountdown() const {
        if (!objects || !objects->Available() || !currentBoard.object_id) return -1;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return -1;
        const auto cd = boardResult.Value()->GetZombieCountdown();
        if (!cd) return -1;
        return cd.Value();
    }
    void AnnounceWave() {
        if (IsTreeOfWisdom()) { AnnounceTreeStatus(); return; }
        const auto context = Context();
        std::string text = "进度：第 " + std::to_string(context.wave) + " 波";
        const int cd = GetWaveCountdown();
        if (cd > 0) {
            text += "，下一波约 " + std::to_string((cd + 99) / 100) + " 秒";
        }
        Say(std::move(text));
    }
    // Shift 单击：按列从左到右播报当前行的僵尸（带状态前缀）与僵王球，
    // 格式：数量；【列号，名称】……（例：2；3，路障僵尸；5，撑杆僵尸）
    void AnnounceRowZombies() {
        std::string view;
        if (!objects || !objects->Available() || !ViewName('z', view)) { Say("本行没有僵尸"); return; }
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) { Say("本行没有僵尸"); return; }
        const tao::Board boardProxy = boardResult.Value();
        std::vector<std::pair<int, std::string>> entries;
        BossBallInfo bossBall;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto zombie = objects->Wrap<tao::Zombie>(handle);
            if (!zombie) return;
            const auto z = zombie.Value();
            if (const auto dead = z->GetDead(); !dead || dead.Value()) return;
            const auto type = z->GetZombieType();
            if (!type) return;
            const int zombieType = int(type.Value().Value());
            CheckBossBall(z, boardProxy, zombieType, bossBall);
            const auto zRow = z->GetRow();
            const auto posX = z->GetPosX();
            const auto posY = z->GetPosY();
            if (!zRow || !posX || !posY || zRow.Value() != row) return;
            // 与格子朗读一致：僵尸判定格 = (posX + 75) 换算列
            const auto zCol = boardProxy->PixelToGridXKeepOnBoard(int(posX.Value() + 75.0f), int(posY.Value()));
            if (!zCol) return;
            entries.emplace_back(zCol.Value(),
                ZombieStatusPrefix(z) + ZombieDisplayName(zombieType, zombieShortNames));
        });
        // 僵王球与僵尸同表播报；列位置解析失败时排最后并注明"列不明"。
        std::string unknownBall;
        if (bossBall.active && bossBall.ballRow == row) {
            const char* ballName = bossBall.isFire ? "僵王火球" : "僵王冰球";
            if (bossBall.ballCol >= 0) entries.emplace_back(bossBall.ballCol, ballName);
            else unknownBall = ballName;
        }
        if (entries.empty() && unknownBall.empty()) { Say("本行没有僵尸"); return; }
        std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string text = std::to_string(entries.size() + (unknownBall.empty() ? 0 : 1));
        for (const auto& [col, name] : entries) {
            text += "；";
            text += std::to_string(col + 1);
            text += "，";
            text += name;
        }
        if (!unknownBall.empty()) text += "；" + unknownBall + "，列不明";
        Say(std::move(text));
    }
    // Shift 双击：播报本行小推车，没有则朗读"无"。
    void AnnounceRowMowers() {
        std::string view;
        if (!objects || !objects->Available() || !ViewName('m', view)) { Say("无"); return; }
        std::string text;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto mower = objects->Wrap<tao::LawnMower>(handle);
            if (!mower) return;
            const auto m = mower.Value();
            if (const auto dead = m->GetDead(); !dead || dead.Value()) return;
            const auto mRow = m->GetRow();
            if (!mRow || mRow.Value() != row) return;
            // 已触发（正在推行）与已压扁的不算可用小推车
            const auto state = m->GetState();
            if (state) {
                const int value = int(state.Value().Value());
                if (value == int(tao::enums::LawnMowerState::MOWER_TRIGGERED.Value()) ||
                    value == int(tao::enums::LawnMowerState::MOWER_SQUISHED.Value())) return;
            }
            const auto type = m->GetMowerType();
            const char* name = "小推车";
            if (type) {
                switch (int(type.Value().Value())) {
                case 1: name = "泳池清洁车"; break; // LAWNMOWER_POOL
                case 3: name = "超级小推车"; break; // LAWNMOWER_SUPER_MOWER
                default: break;
                }
            }
            if (!text.empty()) text += "、";
            text += name;
        });
        Say(text.empty() ? "无" : std::move(text));
    }
    // 选卡界面 Shift：朗读本关出怪列表。
    // 冒险关 1..50 走原版静态方法 Board::CanZombieSpawnOnLevel（与选卡底部僵尸预览同源）；
    // 小游戏/挑战等其余模式的出怪由 Challenge::InitZombieWaves 写进 Board::mZombieAllowed
    // （bool[100]），宿主语义接口不暴露该字段，故经 TaoEngineResolveObject 解出 Board 指针
    // 后直接读；关卡初始化时已填好，无需逐个试玩。名称遵循僵尸简称开关（F6）。
    void AnnounceLevelZombies() {
        std::string list;
        void* board = BoardPointer();
        if (board) {
            for (int type = 0; type < int(tao::enums::ZombieType::NUM_ZOMBIE_TYPES.Value()); ++type) {
                if (!ZombieAllowedAt(board, type)) continue;
                if (!list.empty()) list += "、";
                list += ZombieDisplayName(type, zombieShortNames);
            }
        } else {
            // 指针解析失败时回退冒险关静态表。
            int level = -1;
            if (objects && objects->Available() && currentBoard.object_id)
                if (auto boardProxy = objects->Wrap<tao::Board>(currentBoard))
                    if (const auto value = boardProxy.Value()->GetLevel())
                        level = value.Value();
            if (level >= 1 && level <= 50)
                for (int type = 0; type < int(tao::enums::ZombieType::NUM_ZOMBIE_TYPES.Value()); ++type) {
                    if (!ZombieSpawnsOnLevel(type, level)) continue;
                    if (!list.empty()) list += "、";
                    list += ZombieDisplayName(type, zombieShortNames);
                }
        }
        Say(list.empty() ? "本关出怪未知" : "本关出怪：" + list);
    }
    // 选卡界面双击 Shift：朗读场地与昼夜（如"前院，白天"；花园类场景只有场地名）。
    void AnnounceScene() {
        const char* unknown = "场景未知";
        if (!objects || !objects->Available() || !currentBoard.object_id) { Say(unknown); return; }
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) { Say(unknown); return; }
        const tao::Board boardProxy = boardResult.Value();
        const auto background = boardProxy->GetBackground();
        if (!background) { Say(unknown); return; }
        switch (int(background.Value().Value())) {
        case 6: Say("蘑菇园"); return;   // BACKGROUND_MUSHROOM_GARDEN
        case 7: Say("花园"); return;     // BACKGROUND_GREENHOUSE
        case 8: Say("水族馆"); return;   // BACKGROUND_ZOMBIQUARIUM
        case 9: Say("智慧树"); return;   // BACKGROUND_TREEOFWISDOM
        default: break;
        }
        const char* place = nullptr;
        switch (int(background.Value().Value())) {
        case 0: case 1: place = "前院"; break; // BACKGROUND_1_DAY / 2_NIGHT
        case 2: case 3: place = "后院"; break; // BACKGROUND_3_POOL / 4_FOG
        case 4: case 5: place = "屋顶"; break; // BACKGROUND_5_ROOF / 6_BOSS
        default: break;
        }
        const auto night = boardProxy->StageIsNight();
        const char* time = night && night.Value() ? "黑夜" : "白天";
        Say(place ? std::string(place) + "，" + time : time);
    }
    void CollectAll() {
        std::string view;
        if (objects && objects->Available() && ViewName('c', view)) {
            // 通过 coins 视图按类型筛选：只收集钱、阳光与通关奖励（通关种子包/奖杯），
            // 掉落与传送带的可用种子包(COIN_USABLE_SEED_PACKET)等可放置物不自动收集。
            size_t collected = 0, finalPackets = 0, trophies = 0;
            ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
                auto coin = objects->Wrap<tao::Coin>(handle);
                if (!coin) return;
                const auto item = coin.Value();
                if (const auto dead = item->GetDead(); !dead || dead.Value()) return;
                if (const auto being = item->GetBeingCollected(); being && being.Value()) return;
                const auto type = item->GetCoinType();
                if (!type) return;
                switch (int(type.Value().Value())) {
                case 1: case 2: case 3: case 4: case 5: case 6: // 金银钻与阳光
                    if (Invoke(TAO_BOARD_COLLECT, handle, false)) ++collected;
                    break;
                case 7: // COIN_FINAL_SEED_PACKET 通关种子包
                    if (Invoke(TAO_BOARD_COLLECT, handle, false)) ++finalPackets;
                    break;
                case 8: // COIN_TROPHY 奖杯
                    if (Invoke(TAO_BOARD_COLLECT, handle, false)) ++trophies;
                    break;
                default:
                    break;
                }
            });
            std::string text = "收集 " + std::to_string(collected) + " 个阳光或金币";
            if (finalPackets) text += "，已领取通关种子包";
            if (trophies) text += "，已领取奖杯";
            Say(std::move(text));
            return;
        }
        std::vector<TaoPluginObjectHandle> targets;
        size_t count = 0;
        host->board.target_count(host->board.host_context, currentBoard, TAO_BOARD_COLLECT, &count);
        for (size_t i = 0; i < count && i < 1024; ++i) {
            TaoPluginBoardTargetV2 target{};
            target.struct_size = sizeof(target);
            if (host->board.target_at(host->board.host_context, currentBoard, TAO_BOARD_COLLECT, i, &target) == TAO_PLUGIN_OK)
                targets.push_back(target.target);
        }
        size_t done = 0;
        for (auto target : targets) done += Invoke(TAO_BOARD_COLLECT, target, false);
        Say("收集 " + std::to_string(done) + " 个阳光或金币");
    }
    // 关卡末尾奖励自动收集：以引擎权威判定 IsLevelAward() 为准
    // （通关种子包/奖杯/奖励铲子/图鉴/车钥匙/玉米卷/便条/奖励袋与模式解锁礼物等）。
    // 铲子槽按钮虽同为 COIN_SHOVEL，但不是关卡奖励，不会被误收；
    // 对局中的可用种子包(16)、花瓶(12)、浇花壶(13)、禅境花园礼物(17)、可用巧克力(23)
    // 以及阳光/金币也都不是关卡奖励，保持不动。
    // 注意不能用 Board::mLevelComplete 做门控：奖励物必须先被点击关卡才算完成。
    void AutoCollectRewards() {
        auto gate = [&](std::string reason) { // 临时诊断：门控未通过原因（变化时记录一次）
            if (reason == autoGate) return;
            autoGate = std::move(reason);
            if (log && !autoGate.empty()) log("自动收集等待：" + autoGate);
        };
        if (!objects || !objects->Available()) { gate("对象服务不可用"); return; }
        const auto context = Context();
        if (context.scene != 3 || !context.board.object_id) { gate("非棋盘场景 scene=" + std::to_string(context.scene)); return; }
        if (context.flags & TAO_INPUT_MODAL) { gate("模态对话框"); return; }
        std::string view;
        if (!ViewName('c', view)) { gate("coins 视图不可用"); return; }
        gate("");
        std::vector<std::string> got;
        std::string seen, failed; // 临时诊断：场上奖励硬币类型与领取失败类型
        std::vector<TaoPluginObjectHandle> present;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto coin = objects->Wrap<tao::Coin>(handle);
            if (!coin) return;
            const auto item = coin.Value();
            if (const auto dead = item->GetDead(); !dead || dead.Value()) return;
            const auto award = item->IsLevelAward();
            if (!award || !award.Value()) return; // 只碰引擎判定的关卡奖励物
            present.push_back(handle);
            const auto type = item->GetCoinType();
            const int value = type ? int(type.Value().Value()) : -1;
            const auto posX = item->GetPosX();
            const auto posY = item->GetPosY();
            seen += (seen.empty() ? "" : " ") + std::to_string(value) + "@" +
                std::to_string(posX ? int(posX.Value()) : -1) + "," + std::to_string(posY ? int(posY.Value()) : -1);
            const char* name = RewardName(value);
            const std::string label = name ? name : "奖励物";
            const auto announcedAt = std::find_if(autoAnnounced.begin(), autoAnnounced.end(),
                [&](const TaoPluginObjectHandle& h) { return SameHandle(h, handle); });
            if (const auto being = item->GetBeingCollected(); being && being.Value()) {
                // 领取已被游戏接受、正在飞行收集：此刻才确认播报，
                // 保证领取动作确实进入了原生链路（飞行到位后引擎自己做过关结算）。
                if (announcedAt == autoAnnounced.end()) {
                    autoAnnounced.push_back(handle);
                    if (std::find(got.begin(), got.end(), label) == got.end()) got.emplace_back(label);
                }
                return;
            }
            // 走游戏原生点击链路：Coin::MouseDown 与真实鼠标点击同一入口，
            // 后续的飞行收集与过关结算由引擎自己完成；API 调用失败才回退宿主动作。
            bool done = false;
            if (posX && posY) done = bool(item->MouseDown(int(posX.Value()) + 20, int(posY.Value()) + 20, 1));
            if (!done) done = Invoke(TAO_BOARD_COLLECT, handle, false);
            if (!done)
                failed += (failed.empty() ? "" : " ") + std::to_string(value);
        });
        // 已离场的句柄移出已播报清单（收集完成），同名奖励下次可再播报
        autoAnnounced.erase(std::remove_if(autoAnnounced.begin(), autoAnnounced.end(),
            [&](const TaoPluginObjectHandle& h) {
                return std::none_of(present.begin(), present.end(),
                    [&](const TaoPluginObjectHandle& p) { return SameHandle(p, h); });
            }), autoAnnounced.end());
        if (seen != autoSeen || !failed.empty() && failed != autoFailed) { // 临时诊断
            autoSeen = seen;
            autoFailed = failed;
            if (log) {
                if (!seen.empty()) log("自动收集扫描，场上硬币类型：" + seen);
                if (!failed.empty()) log("自动收集领取失败，类型：" + failed);
            }
        }
        if (got.empty()) return;
        std::string text = "已领取：";
        for (size_t i = 0; i < got.size(); ++i) {
            if (i) text += "、";
            text += got[i];
        }
        Say(std::move(text), false);
    }
    static const char* RewardName(int type) {
        switch (type) {
        case 7: return "通关种子包";    // COIN_FINAL_SEED_PACKET
        case 8: return "奖杯";          // COIN_TROPHY
        case 9: return "铲子";          // COIN_SHOVEL
        case 10: return "图鉴";         // COIN_ALMANAC
        case 11: return "车钥匙";       // COIN_CARKEYS
        case 14: return "玉米卷";       // COIN_TACO
        case 15: return "便条";         // COIN_NOTE
        case 18: return "钱袋";         // COIN_AWARD_MONEY_BAG
        case 19: return "礼物";         // COIN_AWARD_PRESENT
        case 20: return "钻石袋";       // COIN_AWARD_BAG_DIAMOND
        case 21: return "银向日葵奖杯"; // COIN_AWARD_SILVER_SUNFLOWER
        case 22: return "金向日葵奖杯"; // COIN_AWARD_GOLD_SUNFLOWER
        case 24: return "巧克力";       // COIN_AWARD_CHOCOLATE
        case 25: return "迷你游戏礼物"; // COIN_PRESENT_MINIGAMES
        case 26: return "解谜模式礼物"; // COIN_PRESENT_PUZZLE_MODE
        case 27: return "生存模式礼物"; // COIN_PRESENT_SURVIVAL_MODE
        default: return nullptr;
        }
    }
    void ToggleZombieNames() {
        zombieShortNames = !zombieShortNames;
        Say(zombieShortNames ? "僵尸简称已开启" : "僵尸全称已开启");
    }

    // 循环查看僵尸（, / .）：按对象身份排序循环（句柄+generation 混合键，
    // 替代 PvZA11y 的 uniqueID）。光标定位到僵尸所在格并朗读整格内容，与
    // 方向键移动后的行为一致（PvZA11y 的 MoveOnZombieCycle，方便锤击等操作）；
    // 僵尸在场外无法落格时才退化为"场外，状态前缀+名称"播报。
    void CycleZombie(int direction) {
        if (!objects || !objects->Available()) { Say("现在没有僵尸"); return; }
        std::string view;
        if (!ViewName('z', view)) { Say("现在没有僵尸"); return; }
        struct ZombieEntry { uint64_t key; TaoPluginObjectHandle handle; };
        std::vector<ZombieEntry> zombies;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto zombie = objects->Wrap<tao::Zombie>(handle);
            if (!zombie) return;
            const auto z = zombie.Value();
            if (const auto dead = z->GetDead(); !dead || dead.Value()) return;
            zombies.push_back({handle.object_id ^ (handle.generation * UINT64_C(0x9E3779B97F4A7C15)), handle});
        });
        if (zombies.empty()) { Say("现在没有僵尸"); return; }
        std::sort(zombies.begin(), zombies.end(), [](const ZombieEntry& a, const ZombieEntry& b) { return a.key < b.key; });
        size_t current = 0;
        bool found = false;
        for (size_t i = 0; i < zombies.size(); ++i)
            if (zombies[i].key == cycleZombieKey) { current = i; found = true; break; }
        if (found) current = size_t((int(current) + direction + int(zombies.size())) % int(zombies.size()));
        cycleZombieKey = zombies[current].key;
        auto zombie = objects->Wrap<tao::Zombie>(zombies[current].handle);
        if (!zombie) { Say("现在没有僵尸"); return; }
        const auto z = zombie.Value();
        const auto zRow = z->GetRow();
        const auto posX = z->GetPosX();
        const auto type = z->GetZombieType();
        if (!zRow || !posX || !type) { Say("现在没有僵尸"); return; }
        const auto context = Context();
        // 判定列 = (posX + 75) 换算 80px 列（所有背景共用同一横向几何），出界为场外
        const int zombieColumn = int((posX.Value() + 75.0f - 40.0f) / 80.0f);
        if (zombieColumn >= 0 && zombieColumn < context.columns && zRow.Value() >= 0 && zRow.Value() < context.rows) {
            column = zombieColumn;
            row = zRow.Value();
            layer = 0;
            ReadCell();
            return;
        }
        Say("场外，" + ZombieStatusPrefix(z) + ZombieDisplayName(int(type.Value().Value()), zombieShortNames));
    }

    // 老虎机（Slot Machine）模式：S 拉杆（TAO_BOARD_SLOT_PULL，宿主门控关卡
    // 模式并扣 25 阳光）；数字键改拾取奖励掉落的植物卡（TAO_BOARD_SELECT_CARD，
    // 目标是可选种子卡类型的 Coin，与传送带/掉落卡同一宿主路径）。
    bool IsSlotMachineLevel() {
        if (!objects || !objects->Available()) return false;
        auto app = objects->Root<tao::LawnApp>();
        if (!app) return false;
        const auto slot = app.Value()->IsSlotMachineLevel();
        return slot && slot.Value();
    }
    // 老虎机已拉杆次数：Challenge::mSlotMachineRollCount（绑定 slot_machine_roll_count），
    // 每拉一次 +1、关卡内不归零。>0 即"已拉过杆"——此状态后旋转期间停订宿主观测。
    // 老虎机拉杆计数：Challenge::mSlotMachineRollCount（绑定 slot_machine_roll_count），
    // 每拉一次 +1、关卡内不归零；读不到返回 -1。>0 即"已拉过杆"。
    int SlotMachineRollCount() {
        if (!objects || !objects->Available() || !currentBoard.object_id) return -1;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return -1;
        const auto challenge = boardResult.Value()->GetChallenge();
        if (!challenge) return -1;
        const auto rc = challenge.Value()->GetSlotMachineRollCount();
        return rc ? int(rc.Value()) : -1;
    }
    // 读取对象成员（裸 objects 接口，按运行时成员名；失败返回 false）。
    bool ReadMember(TaoPluginObjectHandle object, const char* name, TaoPluginValue& out) {
        out = {};
        out.struct_size = sizeof(out);
        return host->objectsApi.get && object.object_id &&
            host->objectsApi.get(host->objectsApi.host_context, object, tao::AbiString(name), &out) == TAO_PLUGIN_OK;
    }
    // 老虎机底部建议栏是否在显示（MessageWidget::IsBeingDisplayed，安全的布尔读取）。
    bool SlotAdviceVisible() {
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const auto message = boardResult.Value()->GetMessage();
        if (!message) return false;
        const auto shown = message.Value()->IsBeingDisplayed();
        return shown && shown.Value();
    }
    // 游戏底部建议文本（Board::mAdvice 的 label）。SDK 无类型化 getter，按成员名试读；读不到返回空串。
    std::string SlotAdviceText() {
        if (!currentBoard.object_id) return {};
        for (const char* holder : {"advice", "message", "tooltip"}) {
            TaoPluginValue v{};
            if (!ReadMember(currentBoard, holder, v)) continue;
            if (v.kind == TAO_VALUE_STRING) {
                if (const std::string s = CopyView(v.value.string_value); !s.empty()) return s;
            }
            if (v.kind != TAO_VALUE_OBJECT || !v.value.object_value.object_id) continue;
            for (const char* field : {"label", "text", "title"}) {
                TaoPluginValue t{};
                if (ReadMember(v.value.object_value, field, t) && t.kind == TAO_VALUE_STRING) {
                    if (const std::string s = CopyView(t.value.string_value); !s.empty()) return s;
                }
            }
        }
        return {};
    }
    // 老虎机本次结果：优先读游戏建议文本，读不到则由场上掉落的可用种子包合成。
    std::string SlotResultText() {
        if (const std::string advice = SlotAdviceText(); !advice.empty()) return advice;
        std::vector<std::string> names;
        if (objects && objects->Available()) {
            std::string view;
            if (ViewName('c', view))
                ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
                    auto coin = objects->Wrap<tao::Coin>(handle);
                    if (!coin) return;
                    const auto item = coin.Value();
                    if (const auto dead = item->GetDead(); !dead || dead.Value()) return;
                    if (const auto being = item->GetBeingCollected(); !being || being.Value()) return;
                    const auto type = item->GetCoinType();
                    if (!type || int(type.Value().Value()) != int(tao::enums::CoinType::COIN_USABLE_SEED_PACKET.Value())) return;
                    int seedType = -1;
                    if (const auto usable = item->GetUsableSeedType(); usable) seedType = int(usable.Value().Value());
                    names.push_back(seedType >= 0 ? PlantDisplayName(seedType, -1) : "未知植物");
                });
        }
        if (names.empty()) return "没有新植物，再拉一次";
        std::string text = std::to_string(names.size()) + " 株植物：";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) text += "、";
            text += names[i];
        }
        return text;
    }
    void PullSlot() {
        boardFocus = true;
        const int sunBefore = Context().sun;
        if (sunBefore >= 25 && NativePullSlot() && Context().sun < sunBefore) {
            Say("拉动老虎机");
            return;
        }
        // 原生路径不可用（对象服务异常/把手矩形缺失/阳光不足/已在滚动）才退语义拉杆。
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_SLOT_PULL, {}, result)) { Say("拉动老虎机"); return; }
        Say(ReasonText(result.reason));
    }
    // 原生拉杆：点击拉杆把手（Challenge::SlotMachineGetHandleRect，与鼠标同一
    // 入口）。语义 SLOT_PULL 虽返回成功，但跳过原生点击链路上的提示/状态切换，
    // 恢复模拟后约 1 秒进程崩溃（0xC0000005，诊断日志确认崩溃在拉杆调用返回
    // 之后）；原生路径与鼠标操作完全一致，规避该崩溃。
    bool NativePullSlot() {
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const tao::Board board = boardResult.Value();
        const auto challenge = board->GetChallenge();
        if (!challenge) return false;
        const auto rect = challenge.Value()->SlotMachineGetHandleRect();
        if (!rect) return false;
        const auto r = rect.Value();
        return bool(board->MouseDown(int(r.x + r.width / 2), int(r.y + r.height / 2), 1));
    }
    // ===== 宝石迷阵（GAMEMODE_CHALLENGE_BEGHOULED=20）/ 宝石迷阵旋风（=24）/
    // 僵尸水族馆（GAMEMODE_CHALLENGE_ZOMBIQUARIUM=23）=====
    // 这三关没有常规植物种子银行，银行里是特殊操作包：宝石迷阵的"重新洗牌"
    //（54）与"填平弹坑"（55）、植物升级包（普通植物类型）；水族馆的"潜水僵尸"
    //（58）与"奖杯"（59）。与我是僵尸同款理由绕开 target_at(SELECT_SEED)
    //（宿主对 54+ 特殊包类型求名称/可用性会越界），经 objects 的 seed_packets
    // 视图枚举，invoke 按句柄解析目标。
    bool IsBeghouledLevel() {
        return CurrentGameMode() == int(tao::enums::GameMode::GAMEMODE_CHALLENGE_BEGHOULED.Value());
    }
    bool IsBeghouledTwistLevel() {
        return CurrentGameMode() == int(tao::enums::GameMode::GAMEMODE_CHALLENGE_BEGHOULED_TWIST.Value());
    }
    bool IsZombiquariumLevel() {
        return CurrentGameMode() == int(tao::enums::GameMode::GAMEMODE_CHALLENGE_ZOMBIQUARIUM.Value());
    }
    bool HasSpecialBank() {
        return IsBeghouledLevel() || IsBeghouledTwistLevel() || IsZombiquariumLevel();
    }
    // 特殊操作包名称：54/55/58/59 用固定名；宝石迷阵升级包是普通植物类型
    //（双发 7 / 大喷菇 10 / 高坚果 23），加"升级"前缀；其余（56/57 老虎机包）报"特殊卡片"。
    std::string SpecialBankPacketName(int type) {
        switch (type) {
        case 54: return "重新洗牌"; // SEED_BEGHOULED_BUTTON_SHUFFLE
        case 55: return "填平弹坑"; // SEED_BEGHOULED_BUTTON_CRATER
        case 58: return "潜水僵尸"; // SEED_ZOMBIQUARIUM_SNORKLE
        case 59: return "奖杯";     // SEED_ZOMBIQUARIUM_TROPHY
        default: break;
        }
        if (type >= 0 && type < 53) return "升级" + PlantDisplayName(type, -1); // NUM_SEED_TYPES=53
        return "特殊卡片";
    }
    // 特殊操作包费用（阳光）：优先 Board::GetCurrentPlantCost 运行时真实值
    //（exe 对宝石迷阵按钮/升级包、水族馆包有专门分支，与我是僵尸僵尸包同路径）；
    // 取不到回退内嵌表，再取不到返回 -1。
    int SpecialBankPacketCost(int type) {
        if (objects && objects->Available()) {
            if (auto board = objects->Wrap<tao::Board>(currentBoard)) {
                const auto cost = board.Value()->GetCurrentPlantCost(
                    tao::SeedType{type}, tao::enums::SeedType::SEED_NONE);
                if (cost && cost.Value() > 0) return cost.Value();
            }
        }
        switch (type) {
        case 54: return 100;  // 重新洗牌
        case 55: return 200;  // 填平弹坑
        case 58: return 100;  // 潜水僵尸
        case 59: return 1000; // 奖杯
        case 7:  return 1000; // 升级双发射手
        case 10: return 500;  // 升级大喷菇
        case 23: return 250;  // 升级高坚果
        default: return -1;
        }
    }
    // 数字键/循环键选特殊操作包：这些包是即时按钮（洗牌/填坑/买僵尸/升级点下
    // 即生效，没有"选中再确认"），为避免误触改为**单击朗读、双击执行**。
    // 单击：朗读名称+运行时费用，附"冷却中"/"阳光不足"状态。双击（同一包
    // 450ms 内）：经 Challenge 代理调宿主原生点击处理 beghouled_packet_clicked /
    // zombiquarium_packet_clicked（与鼠标点按钮同一入口，自带扣费/冷却门禁），
    // 按执行前后阳光变化判成败。宿主语义 SELECT_SEED 对这些包不生效（实测）。
    void SelectSpecialBankPacket(size_t index) {
        const auto packets = BankPackets();
        if (index >= packets.size()) { Say("没有这个操作"); return; }
        const auto& pk = packets[index];
        seedIndex = pk.slot;
        std::string text = SpecialBankPacketName(pk.type);
        const int cost = SpecialBankPacketCost(pk.type);
        if (cost > 0) text += "，" + std::to_string(cost) + "阳光";
        if (objects && objects->Available()) {
            if (auto packet = objects->Wrap<tao::SeedPacket>(pk.handle)) {
                const auto counter = packet.Value()->GetRefreshCounter();
                if (counter && counter.Value() > 0) text += "，冷却中";
            }
        }
        const int sunBefore = Context().sun;
        if (cost > 0 && sunBefore < cost) text += "，阳光不足";
        const auto now = std::chrono::steady_clock::now();
        const bool doublePress = specialBankLastSlot == pk.slot &&
            now - specialBankLastPressedAt < std::chrono::milliseconds(450);
        specialBankLastSlot = pk.slot;
        specialBankLastPressedAt = now;
        if (!doublePress) { Say(text); return; }
        boardFocus = true;
        bool called = false;
        if (objects && objects->Available()) {
            if (auto boardResult = objects->Wrap<tao::Board>(currentBoard)) {
                if (const auto challenge = boardResult.Value()->GetChallenge()) {
                    if (auto packet = objects->Wrap<tao::SeedPacket>(pk.handle)) {
                        called = IsZombiquariumLevel()
                            ? bool(challenge.Value()->ZombiquariumPacketClicked(packet.Value()))
                            : bool(challenge.Value()->BeghouledPacketClicked(packet.Value()));
                    }
                }
            }
        }
        if (!called) { Say(text + "，执行不可用"); return; }
        Say(Context().sun < sunBefore ? text + "，已执行" : text + "，未能执行");
    }
    // 宝石迷阵：Ctrl+方向键把当前格植物拖向该方向（宿主 TAO_BOARD_SWAP：
    // target=植物句柄、column/row=目标格，宿主调 Challenge::BeghouledSwap）。
    // 交换未凑成匹配时游戏会把植物弹回，宿主返回 TAO_ACTION_REQUIREMENT。
    void DragPlant(int dx, int dy) {
        boardFocus = true;
        TaoPluginObjectHandle plant{};
        if (objects && objects->Available()) {
            if (auto boardProxy = objects->Wrap<tao::Board>(currentBoard)) {
                auto top = boardProxy.Value()->GetTopPlantAt(column, row,
                    tao::enums::PlantPriority::TOPPLANT_ONLY_NORMAL_POSITION);
                if (top && !top.Value().Empty()) plant = top.Value().Handle();
            }
        }
        if (!plant.object_id) { Say("此格没有植物"); return; }
        TaoPluginBoardActionRequestV2 request{};
        request.struct_size = sizeof(request);
        request.board = currentBoard;
        request.action = TAO_BOARD_SWAP;
        request.target = plant;
        request.column = column + dx;
        request.row = row + dy;
        TaoPluginBoardActionResultV2 result{};
        result.struct_size = sizeof(result);
        if (host->board.invoke(host->board.host_context, &request, &result) == TAO_PLUGIN_OK) {
            matchCueGridDirty = true; // 交换后网格已变（消除/下落还会异步变化）
            MoveCursor(dx, dy); // 光标跟随被拖动的植物
            ReadCell();
            return;
        }
        Say(result.reason == TAO_ACTION_REQUIREMENT ? "没有形成匹配" : ReasonText(result.reason));
    }
    // 宝石迷阵旋风：回车旋转当前格 2x2 植物（宿主 TAO_BOARD_ROTATE，
    // column/row 为 2x2 区域左上角，宿主调 Challenge::BeghouledTwistAt）。
    void RotateCurrent() {
        boardFocus = true;
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_ROTATE, {}, result)) { matchCueGridDirty = true; ReadCell(); }
        else Say(ReasonText(result.reason));
    }
    // 僵尸水族馆：回车在当前格投喂鱼食（宿主 TAO_BOARD_FEED，扣 5 阳光，
    // column/row 换算像素落点；僵尸会游向鱼食）。
    void FeedCurrent() {
        boardFocus = true;
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_FEED, {}, result)) Say("投喂鱼食");
        else Say(ReasonText(result.reason));
    }
    // 宝石迷阵（经典）：回车不直接操作，提示拖动方式。
    void BeghouledEnterHint() {
        boardFocus = true;
        Say("按住 Ctrl 加方向键，把当前格植物拖向该方向凑成三消");
    }
    // 宝石迷阵/水族馆的阳光与金币自动收集：这些关卡没有向日葵，阳光全靠
    // 拾取场上掉落，手动定位对视障用户不可行（水族馆阳光还落不进网格）。
    // 静默收集银币/金币/钻石/阳光类（类型 1..6），不播报，与关卡奖励收集同节流。
    void AutoCollectSunCoins() {
        if (!HasSpecialBank()) return;
        if (!objects || !objects->Available()) return;
        const auto context = Context();
        if (context.scene != 3 || !context.board.object_id) return;
        if (context.flags & TAO_INPUT_MODAL) return;
        std::string view;
        if (!ViewName('c', view)) return;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto coin = objects->Wrap<tao::Coin>(handle);
            if (!coin) return;
            const auto item = coin.Value();
            if (const auto dead = item->GetDead(); !dead || dead.Value()) return;
            if (const auto being = item->GetBeingCollected(); being && being.Value()) return;
            const auto type = item->GetCoinType();
            if (!type) return;
            const int value = int(type.Value().Value());
            if (value < 1 || value > 6) return; // 银/金/钻/阳光/小阳光/大阳光
            const auto posX = item->GetPosX();
            const auto posY = item->GetPosY();
            if (posX && posY) item->MouseDown(int(posX.Value()) + 20, int(posY.Value()) + 20, 1);
        });
    }
    // OnUpdate 每 tick 喂入（NoteTick），供 ReadCell 等无 tick 上下文的提示音用。
    void NoteTick(uint64_t tick) { boardTick = tick; }
    // 宝石迷阵/旋风"可达匹配"提示音：浏览到存在可行拖动（旋风为可行旋转）的
    // 格子时播 SOUND_BLEEP（游戏内置音效，resource_id 直接引用）。同一格不重复响
    //（F2 重读静默），离开再回来重播；交换/旋转后网格缓存作废重估。
    void MatchCue() {
        if (column != matchCueCheckColumn || row != matchCueCheckRow || matchCueGridDirty) {
            matchCueCheckColumn = column;
            matchCueCheckRow = row;
            matchCueGridDirty = false;
            matchCueCheckResult = IsBeghouledTwistLevel() ? TwistMatchAvailable() : SwapMatchAvailable();
        }
        if (!matchCueCheckResult) { matchCueBeepColumn = matchCueBeepRow = -1; return; }
        if (column == matchCueBeepColumn && row == matchCueBeepRow) return;
        matchCueBeepColumn = column;
        matchCueBeepRow = row;
        PlayCueSound("SOUND_CHIME", boardTick);
    }
    // 三消判定工具：从 (c0,r0) 沿 (dx,dy) 数连续同类型格数（最多 2，凑 3 连够用）。
    template <typename Get>
    static int MatchRun(Get&& get, int c0, int r0, int dx, int dy, int type) {
        int n = 0;
        for (int i = 1; i <= 2; ++i) { if (get(c0 + dx * i, r0 + dy * i) == type) ++n; else break; }
        return n;
    }
    // (c0,r0) 处类型 type 是否凑成横/竖 3 连。
    template <typename Get>
    static bool MatchLineAt(Get&& get, int c0, int r0, int type) {
        if (type < 0) return false;
        return MatchRun(get, c0, r0, 1, 0, type) + MatchRun(get, c0, r0, -1, 0, type) + 1 >= 3 ||
               MatchRun(get, c0, r0, 0, 1, type) + MatchRun(get, c0, r0, 0, -1, type) + 1 >= 3;
    }
    // 把 (c0..c1)x(r0..r1) 的普通位植物类型填进连续数组 out（行优先，宽 width），
    // 越界与无植物格保持 -1。匹配模拟专用（一次取齐，避免逐格重复 Wrap Board）。
    void FillBeghouledGrid(int c0, int c1, int r0, int r1, int* out, int width) {
        const int height = r1 - r0 + 1;
        for (int i = 0; i < width * height; ++i) out[i] = -1;
        if (!objects || !objects->Available()) return;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return;
        const tao::Board boardProxy = boardResult.Value();
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                if (c < 0 || r < 0) continue;
                auto plant = boardProxy->GetTopPlantAt(c, r,
                    tao::enums::PlantPriority::TOPPLANT_ONLY_NORMAL_POSITION);
                if (!plant || plant.Value().Empty()) continue;
                const auto type = plant.Value()->GetPlantType();
                if (type) out[(r - r0) * width + (c - c0)] = int(type.Value().Value());
            }
    }
    // 经典宝石迷阵：当前格四个方向交换后能否凑成 3 连（弹坑/空格不可参与）。
    bool SwapMatchAvailable() {
        int g[5][5]; // 以光标为中心半径 2：3 连判定最多向两侧延伸 2 格
        FillBeghouledGrid(column - 2, column + 2, row - 2, row + 2, &g[0][0], 5);
        auto at = [&](int c, int r) -> int {
            const int gc = c - (column - 2), gr = r - (row - 2);
            if (gc < 0 || gc >= 5 || gr < 0 || gr >= 5) return -1;
            return g[gr][gc];
        };
        const int self = at(column, row);
        if (self < 0) return false;
        static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : dirs) {
            const int nc = column + d[0], nr = row + d[1];
            const int other = at(nc, nr);
            if (other < 0 || other == self) continue;
            auto get = [&](int c, int r) -> int { // 交换后的取格视图
                if (c == column && r == row) return other;
                if (c == nc && r == nr) return self;
                return at(c, r);
            };
            if (MatchLineAt(get, column, row, other) || MatchLineAt(get, nc, nr, self)) return true;
        }
        return false;
    }
    // 宝石迷阵旋风：旋转当前格 2x2（顺时针，宿主 BeghouledTwistAt 同款）后
    // 四个位置任一能凑成 3 连即可行。
    bool TwistMatchAvailable() {
        int g[6][6]; // 2x2 块向外延伸 2 格
        FillBeghouledGrid(column - 2, column + 3, row - 2, row + 3, &g[0][0], 6);
        auto at = [&](int c, int r) -> int {
            const int gc = c - (column - 2), gr = r - (row - 2);
            if (gc < 0 || gc >= 6 || gr < 0 || gr >= 6) return -1;
            return g[gr][gc];
        };
        const int topLeft = at(column, row), topRight = at(column + 1, row);
        const int bottomLeft = at(column, row + 1), bottomRight = at(column + 1, row + 1);
        if (topLeft < 0 || topRight < 0 || bottomLeft < 0 || bottomRight < 0) return false;
        auto get = [&](int c, int r) -> int { // 顺时针旋转后的取格视图
            if (c == column && r == row) return bottomLeft;
            if (c == column + 1 && r == row) return topLeft;
            if (c == column && r == row + 1) return bottomRight;
            if (c == column + 1 && r == row + 1) return topRight;
            return at(c, r);
        };
        return MatchLineAt(get, column, row, bottomLeft) ||
               MatchLineAt(get, column + 1, row, topLeft) ||
               MatchLineAt(get, column, row + 1, bottomRight) ||
               MatchLineAt(get, column + 1, row + 1, topRight);
    }
    // 场上"可用种子卡"（COIN_USABLE_SEED_PACKET 掉落卡，按出现顺序）。老虎机奖励
    // 掉卡与花瓶砸出的植物卡共用这条枚举路径。
    // 不走 host->board.target_at(SELECT_CARD)：宿主该路径在无卡可枚举时崩溃
    // （实测 0xC0000005，诊断日志止于 target_at 内）。改用 objects coins 视图自行
    // 枚举（与 AutoCollectRewards 同一条安全路径），句柄直接喂给 BoardInvoke ——
    // invoke 按句柄解析目标，不经过 target_at 的枚举逻辑。
    std::vector<std::pair<TaoPluginObjectHandle, std::string>> DroppedCards() {
        std::vector<std::pair<TaoPluginObjectHandle, std::string>> cards;
        if (!objects || !objects->Available()) return cards;
        std::string view;
        if (!ViewName('c', view)) return cards;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto coin = objects->Wrap<tao::Coin>(handle);
            if (!coin) return;
            const auto item = coin.Value();
            if (const auto dead = item->GetDead(); !dead || dead.Value()) return;
            if (const auto being = item->GetBeingCollected(); !being || being.Value()) return;
            const auto type = item->GetCoinType();
            if (!type || int(type.Value().Value()) != int(tao::enums::CoinType::COIN_USABLE_SEED_PACKET.Value())) return;
            // 名称取 Coin::mUsableSeedType（与宿主 target_at(SELECT_CARD) 用的同一字段）；
            // GetFinalSeedPacketType 在老虎机掉卡时可能返回非植物类型（SEED_SLOT_MACHINE_*），
            // 会退化成"未知植物"，故优先用 usable 字段，取不到再退回 final。
            int seedType = -1;
            if (const auto usable = item->GetUsableSeedType(); usable) seedType = int(usable.Value().Value());
            if (seedType < 0) {
                if (const auto fin = item->GetFinalSeedPacketType(); fin) seedType = int(fin.Value().Value());
            }
            cards.emplace_back(handle, seedType >= 0 ? PlantDisplayName(seedType, -1) : "未知植物");
        });
        return cards;
    }
    // 拾取掉落植物卡：成功后光标持卡（CURSOR_TYPE_PLANT_FROM_USABLE_COIN），
    // 再由确认键种到当前格（宿主 TAO_BOARD_PLANT 支持该光标）。手上已持卡时
    // 宿主 SELECT_CARD 自己先放回（RefreshSeedPacketFromCursor）；持铲则先走
    // 原生路径放回铲子（语义 CANCEL 不放回真实铲子，同 SelectSeed 排障结论）。
    void SelectCard(size_t index) {
        const auto cards = DroppedCards();
        if (index >= cards.size()) {
            Say("没有这张植物卡");
            return;
        }
        const auto& card = cards[index];
        const std::string& name = card.second;
        if (Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value()))
            NativeReturnShovel();
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_SELECT_CARD, card.first, result)) {
            boardFocus = true;
            Say(name + "，已拾取");
        } else {
            Say(name + "，" + ReasonText(result.reason));
        }
    }
    // 我是僵尸（I, Zombie）解谜：种子银行里是僵尸包（mPacketType = SEED_ZOMBIE_*，
    // 类型 60+），不是植物。宿主 a11y 的 target_at(SELECT_SEED) 对这类包**名称错位**
    // （槽 i 报槽 i+1 的僵尸名），invoke 按索引选卡对首包之外的包直接崩
    // （0xC0000005，2026-10-08 实机定论）。实现绕开整条 a11y SELECT_SEED 路径：
    // objects 的 seed_packets 视图枚举包对象 → GetPacketType 经游戏导出的
    // Challenge::IZombieSeedTypeToZombieType 映射僵尸类型 → RuntimeZombieName 播名，
    // 费用走 Board::GetCurrentPlantCost（对僵尸包正确），选卡按句柄 invoke 或
    // SeedPacket::Activate 语义执行。详见 SelectZombiePacket。
    bool IsIZombieLevel() {
        if (objects && objects->Available()) {
            if (auto app = objects->Root<tao::LawnApp>()) {
                const auto iz = app.Value()->IsIZombieLevel();
                if (iz) return iz.Value();
            }
        }
        // 回退：按游戏模式判定（GAMEMODE_PUZZLE_I_ZOMBIE_1..ENDLESS = 61..70）。
        const int mode = CurrentGameMode();
        return mode >= int(tao::enums::GameMode::GAMEMODE_PUZZLE_I_ZOMBIE_1.Value()) &&
            mode <= int(tao::enums::GameMode::GAMEMODE_PUZZLE_I_ZOMBIE_ENDLESS.Value());
    }
    // 我是僵尸僵尸包（SEED_ZOMBIE_* 60..74）的完整语义实现：不走宿主 a11y 的
    // target_at(SELECT_SEED)（名称错位、疑似越界崩溃源），改为 objects 枚举
    // seed_packets 视图 + 生成代理 getter + 游戏导出的映射函数 + 运行时僵尸名。
    IzombieSeedTypeToZombieTypeFn izombieMap = nullptr;
    bool izombieMapProbed = false;
    bool translateReady = false;
    int ZombieTypeFromPacket(int packetType) {
        if (!izombieMapProbed) {
            izombieMapProbed = true;
#ifdef _WIN32
            if (const HMODULE exe = GetModuleHandleW(nullptr)) {
                izombieMap = reinterpret_cast<IzombieSeedTypeToZombieTypeFn>(
                    reinterpret_cast<void*>(GetProcAddress(exe, kIzombieSeedTypeToZombieType)));
                translateReady = GetProcAddress(exe, kTodStringTranslateExport) != nullptr;
            }
#else
            izombieMap = reinterpret_cast<IzombieSeedTypeToZombieTypeFn>(
                dlsym(RTLD_DEFAULT, kIzombieSeedTypeToZombieType));
            translateReady = true;
#endif
            if (log && !izombieMap) log("我是僵尸：宿主未导出 IZombieSeedTypeToZombieType，回退未知命名");
        }
        if (!izombieMap) return -1;
        const int zt = izombieMap(packetType);
        return zt >= 0 && zt < 100 ? zt : -1;
    }
    std::string ZombiePacketName(int packetType) {
        const int zt = ZombieTypeFromPacket(packetType);
        if (zt < 0) return "未知僵尸包";
        std::string name = RuntimeZombieName(zt);
        if (!name.empty() && !LooksLocalized(name)) {
            // ZombieDefinition 给出的是本地化键：经 TodStringTranslate 取译文
            //（不带括号/带括号各试一次），都失败回退内置表。
            if (translateReady) {
                const std::string bracketed = "[" + name + "]";
                for (const std::string& key : {name, bracketed}) {
                    const std::string translated = TodStringTranslate(key);
                    if (!translated.empty() && translated != key && LooksLocalized(translated))
                        return translated;
                }
            }
            name = ZombieNameFallback(zt);
        }
        return name.empty() ? ZombieNameFallback(zt) : name;
    }
    // 费用：Board::GetCurrentPlantCost（生成代理；宿主 a11y 的 SELECT_SEED 费用
    // 即来自该函数，对僵尸包正确——实机已验证）。取不到返回 -1。
    int ZombiePacketCost(int packetType) {
        if (objects && objects->Available()) {
            if (auto board = objects->Wrap<tao::Board>(currentBoard)) {
                const auto cost = board.Value()->GetCurrentPlantCost(
                    tao::SeedType{packetType}, tao::enums::SeedType::SEED_NONE);
                if (cost) return cost.Value();
            }
        }
        return -1;
    }
    std::string ZombiePacketCostText(int packetType) {
        const int cost = ZombiePacketCost(packetType);
        return cost > 0 ? std::to_string(cost) + "阳光" : std::string{};
    }
    struct BankPacket { TaoPluginObjectHandle handle; int slot; int type; };
    // 种子银行僵尸包：经 objects 的 seed_packets 视图枚举（与 zombies/coins 同一
    // 命名族），GetIndex 排序。视图缺失时返回空（调用方回退提示）。
    std::vector<BankPacket> BankPackets() {
        std::vector<BankPacket> list;
        std::string view;
        if (!objects || !objects->Available() || !ViewName('p', view)) {
            if (log) log("我是僵尸：seed_packets 视图不可用");
            return list;
        }
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto packet = objects->Wrap<tao::SeedPacket>(handle);
            if (!packet) return;
            const auto type = packet.Value()->GetPacketType();
            const auto slot = packet.Value()->GetIndex();
            if (!type || !slot) return;
            list.push_back({handle, int(slot.Value()), int(type.Value().Value())});
        });
        std::sort(list.begin(), list.end(), [](const BankPacket& a, const BankPacket& b) { return a.slot < b.slot; });
        return list;
    }
    // 数字键/循环键选僵尸包：播报（名称+费用）后按句柄语义选卡。执行链：
    // InvokeRaw(SELECT_SEED, 包句柄)（与掉落卡 SELECT_CARD 同款按句柄解析，不经过
    // target_at 枚举）→ 失败退 SeedPacket::Activate 语义 → 再失败只播报原因。
    void SelectZombiePacket(size_t index) {
        const auto packets = BankPackets();
        if (index >= packets.size()) { Say("没有这个僵尸包"); return; }
        const auto& pk = packets[index];
        seedIndex = pk.slot;
        std::string text = ZombiePacketName(pk.type);
        if (const std::string cost = ZombiePacketCostText(pk.type); !cost.empty()) text += "，" + cost;
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_SELECT_SEED, pk.handle, result)) {
            boardFocus = true;
            Say(text);
            return;
        }
        if (objects && objects->Available()) {
            if (auto packet = objects->Wrap<tao::SeedPacket>(pk.handle)) {
                if (packet.Value()->Activate()) {
                    boardFocus = true;
                    Say(text);
                    return;
                }
            }
        }
        Say(text + "，" + ReasonText(result.reason));
    }
    // 统一的数字卡槽（0 起）：种子银行在前（SELECT_SEED），场上掉落卡在后
    // （SELECT_CARD）。砸花瓶关卡种子银行与掉落卡并存，数字键与循环键都走这里；
    // 老虎机无种子银行（仅掉落卡）。越界时回到种子银行的"没有这个种子包"提示。
    void SelectCardSlot(size_t index) {
        // 我是僵尸：整条链路走 objects 枚举的僵尸包实现（见 SelectZombiePacket）。
        if (IsIZombieLevel()) { SelectZombiePacket(index); return; }
        // 宝石迷阵/旋风/水族馆：特殊操作包（洗牌/填坑/升级/买僵尸/奖杯）。
        if (HasSpecialBank()) { SelectSpecialBankPacket(index); return; }
        seedIndex = int(index);
        const size_t bank = IsSlotMachineLevel() ? 0 : SeedBankCount();
        if (index < bank) { SelectSeed(index); return; }
        if (index - bank < DroppedCards().size()) { SelectCard(index - bank); return; }
        if (bank) SelectSeed(index); // 越界：走种子银行的越界提示
        else SelectCard(index);
    }

    // Whack-A-Zombie（打僵尸）模式：锤子光标常驻手上（等同普通关卡的空光标），
    // 确认键 = 锤击当前格（TAO_BOARD_WHACK 取目标格中最上层活僵尸，
    // 宿主文档限定打僵尸模式且锤子可用）。
    bool IsWhackLevel() {
        if (!objects || !objects->Available()) return false;
        auto app = objects->Root<tao::LawnApp>();
        if (!app) return false;
        const auto whack = app.Value()->IsWhackAZombieLevel();
        return whack && whack.Value();
    }
    // 锤击全程静默：命中与挥空都由游戏自身挥锤/击杀音效反馈，不播报任何结果。
    void WhackCurrent() {
        boardFocus = true;
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_WHACK, {}, result)) return;
        // 语义锤击失败（挥空或被门控）退到格子中心原生点击：
        // 打僵尸关卡持锤左键即锤击（与鼠标同一入口）。
        NativeWhackCell();
    }

    // 砸花瓶（Vasebreaker/Scary Potter 解谜）模式：宿主 TAO_BOARD_BREAK_VASE 限定本关
    // 为 IsScaryPotterLevel、目标格有花瓶（GridItem GRIDITEM_SCARY_POT）。光标为空时回车
    // = 砸当前格花瓶；砸出的植物卡落在场上，由数字键拾取（见 SelectCard/DroppedCards），
    // 拾到后回车种下（CURSOR_TYPE_PLANT_FROM_USABLE_COIN，宿主 TAO_BOARD_PLANT 支持）。
    bool IsVaseLevel() {
        if (!objects || !objects->Available()) return false;
        auto app = objects->Root<tao::LawnApp>();
        if (!app) return false;
        const auto vase = app.Value()->IsScaryPotterLevel();
        return vase && vase.Value();
    }
    // 当前格的未破花瓶句柄（GRIDITEM_SCARY_POT）；无则返回空句柄。
    TaoPluginObjectHandle VaseAtCursor() {
        TaoPluginObjectHandle found{};
        if (!objects || !objects->Available()) return found;
        std::string view;
        if (!ViewName('g', view)) return found;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            if (found.object_id) return;
            auto item = objects->Wrap<tao::GridItem>(handle);
            if (!item) return;
            const auto gridItem = item.Value();
            if (const auto dead = gridItem->GetDead(); !dead || dead.Value()) return;
            const auto gridX = gridItem->GetGridX();
            const auto gridY = gridItem->GetGridY();
            const auto type = gridItem->GetType();
            if (!gridX || !gridY || !type) return;
            if (gridX.Value() != column || gridY.Value() != row) return;
            if (int(type.Value().Value()) != int(tao::enums::GridItemType::GRIDITEM_SCARY_POT.Value())) return;
            found = handle;
        });
        return found;
    }
    void BreakVase() {
        boardFocus = true;
        const TaoPluginObjectHandle vase = VaseAtCursor();
        if (!vase.object_id) { Say("此格没有花瓶"); return; }
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_BREAK_VASE, vase, result)) Say("砸开花瓶");
        else Say(ReasonText(result.reason));
    }

    // ===== 禅境花园（GAMEMODE_CHALLENGE_ZEN_GARDEN=43，蘑菇园/水族园同一模式） =====
    // 宿主无障碍层已把工具条、植物（value=需求描述）、臭臭、搬移落点暴露成语义项
    // （ModAccessibilityUi.inl）。mod 的数字键/回车复刻宿主条目 activate 的原生链路：
    // 拾取工具 = RefreshSeedPacketFromCursor + PickUpTool（工具条项），
    // 对植物使用 = ZenGarden::FeedPlant / UseToolOnPlant（植物项），
    // 搬移落点 = MovePlant / PlaceWheelbarrowPlant + ClearCursor（落点项）；
    // 需求读取 = GetDynamicPlantNeed / GetPlantsNeed(PottedPlantFromIndex)（与宿主同源）。
    bool IsZenGarden() {
        if (!currentBoard.object_id) return false;
        ProbeZenMode();
        return zenGarden;
    }
    // 智慧树（GAMEMODE_TREE_OF_WISDOM=50）：从禅境花园进入后 GameMode 43→50，
    // 宿主场景仍是 scene=3，不拦会被当常规关卡（方向键读空格、回车种植、
    // 数字键选卡槽）。棋盘探针见 ProbeZenMode。
    bool IsTreeOfWisdom() {
        if (!currentBoard.object_id) return false;
        ProbeZenMode();
        return treeOfWisdom;
    }
    // 花园模式探针：进出花园伴随换棋盘（探针随棋盘失效重求），但花园内部
    // "下一花园"切到智慧树/智慧树返回花园是同一棋盘上 43↔50 就地切换，
    // 所以一旦落入花园家族（43/50）就每次重求模式，不能再信缓存。
    void ProbeZenMode() {
        if (zenProbeValid && !zenGarden && !treeOfWisdom) return;
        zenProbeValid = true;
        const int mode = CurrentGameMode();
        zenGarden = mode == int(tao::enums::GameMode::GAMEMODE_CHALLENGE_ZEN_GARDEN.Value());
        treeOfWisdom = mode == int(tao::enums::GameMode::GAMEMODE_TREE_OF_WISDOM.Value());
    }
    // ZenGarden 对象代理（LawnApp::zen_garden，随棋盘缓存失败不缓存）。
    bool ResolveGarden(tao::ZenGarden& out) {
        if (!objects || !objects->Available()) return false;
        auto app = objects->Root<tao::LawnApp>();
        if (!app) return false;
        auto garden = app.Value()->GetZenGarden();
        if (!garden || garden.Value().Empty()) return false;
        out = garden.Value();
        return true;
    }
    // 植物需求（PottedPlantNeed 裸值；读不到返回 -1）。
    int GardenNeed(const tao::Plant& plant) {
        tao::ZenGarden garden;
        if (!ResolveGarden(garden)) return -1;
        const auto dynamic = plant->IsDynamicContent();
        if (dynamic && dynamic.Value()) {
            const auto need = garden->GetDynamicPlantNeed(plant);
            return need ? int(need.Value().Value()) : -1;
        }
        const auto index = plant->GetPottedPlantIndex();
        if (!index || index.Value() < 0) return -1;
        const auto potted = garden->PottedPlantFromIndex(index.Value());
        if (!potted) return -1;
        const auto need = garden->GetPlantsNeed(potted.Value());
        return need ? int(need.Value().Value()) : -1;
    }
    // 需求动词（浇水/施肥/杀虫/放音乐）；0=无需求、-1=读取失败均返回空。
    static const char* GardenNeedName(int need) {
        switch (need) {
        case 1: return "浇水";   // PLANTNEED_WATER
        case 2: return "施肥";   // PLANTNEED_FERTILIZER
        case 3: return "杀虫";   // PLANTNEED_BUGSPRAY
        case 4: return "放音乐"; // PLANTNEED_PHONOGRAPH
        default: return nullptr;
        }
    }
    static int GardenToolIndex(int cursor) {
        for (int i = 0; i < int(std::size(kZenTools)); ++i)
            if (kZenTools[i].cursor == cursor) return i;
        return -1;
    }
    // 数字键 1..8 选择工具（固定映射，见 kZenTools）：原生拾取，未解锁/被门禁时
    // PickUpTool 不改光标，按结果播报。数字键 0 = 前往下一个花园（宿主"下一
    // 花园"按钮条目的原生终点 GotoNextGarden，门禁 CanUseGameObject 同款）。
    void SelectGardenTool(int digit) {
        if (digit == 9) { GotoNextGarden(); return; }
        if (digit < 0 || digit >= int(std::size(kZenTools))) { Say("没有这个工具"); return; }
        if (!objects || !objects->Available() || !currentBoard.object_id) { Say("工具不可用"); return; }
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) { Say("工具不可用"); return; }
        const tao::Board board = boardResult.Value();
        const ZenToolDef& tool = kZenTools[digit];
        const auto usable = board->CanUseGameObject(tao::GameObjectType{tool.object});
        if (!usable || !usable.Value()) { Say(std::string(tool.name) + "还没有解锁"); return; }
        // 与宿主工具条条目 activate 同一原生链路：手上种子包先放回银行，再拾取工具。
        board->RefreshSeedPacketFromCursor();
        board->PickUpTool(tao::GameObjectType{tool.object});
        gardenCarried = {};
        if (Context().cursor == tool.cursor) {
            boardFocus = true;
            Say(std::string("已拿起") + tool.name);
        } else {
            Say(std::string("现在不能拿起") + tool.name);
        }
    }
    // 当前格的花园植物（任意层）。
    bool GardenPlantAtCursor(tao::Plant& out) {
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const auto plant = boardResult.Value()->GetTopPlantAt(column, row,
            tao::enums::PlantPriority::TOPPLANT_ANY);
        if (!plant || plant.Value().Empty()) return false;
        out = plant.Value();
        return true;
    }
    // 回车：使用手上的工具，或放下搬移中的植物。
    void UseGardenTool() {
        boardFocus = true;
        const int cursor = Context().cursor;
        if (cursor == int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_GLOVE.Value()) ||
            cursor == int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_WHEEL_BARROW.Value())) {
            PlaceCarriedPlant();
            return;
        }
        // 手推车装着植物时光标仍停在普通/手推车型（装车后游戏不切换成搬移光标），
        // 此时回车应卸车放到目标格，而不是按手持工具判定"此格没有植物"。
        if (cursor == int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value()) ||
            cursor == int(tao::enums::CursorType::CURSOR_TYPE_WHEEELBARROW.Value())) {
            tao::ZenGarden garden;
            if (ResolveGarden(garden)) {
                const auto barrow = garden->GetPottedPlantInWheelbarrow();
                if (barrow && !barrow.Value().Empty()) { PlaceCarriedPlant(); return; }
            }
        }
        const int toolIndex = GardenToolIndex(cursor);
        // 空手回车：叫醒睡觉的臭臭（宿主臭臭条目 activate 同款调用），
        // 臭臭醒着/未购买时落回工具提示。
        if (toolIndex < 0) {
            tao::ZenGarden garden;
            if (ResolveGarden(garden)) {
                const auto sleeping = garden->IsStinkySleeping();
                if (sleeping && sleeping.Value()) {
                    garden->WakeStinky();
                    const auto after = garden->IsStinkySleeping();
                    if (after && !after.Value()) { Say("已叫醒臭臭"); return; }
                }
            }
            Say("请先用数字键选择工具");
            return;
        }
        const ZenToolDef& tool = kZenTools[toolIndex];
        tao::ZenGarden garden;
        if (!ResolveGarden(garden)) { Say("花园对象不可用"); return; }
        tao::Plant plant;
        if (!GardenPlantAtCursor(plant)) {
            // 巧克力对空格回车：喂臭臭（宿主臭臭条目 activate 的巧克力分支）。
            const auto stinky = garden->GetStinky();
            if (tool.cursor == int(tao::enums::CursorType::CURSOR_TYPE_CHOCOLATE.Value()) &&
                stinky && !stinky.Value().Empty()) {
                const auto fed = garden->FeedStinkyChocolate();
                Say(fed && fed.Value() ? "已给臭臭喂巧克力" : "臭臭现在不想吃巧克力");
                return;
            }
            Say("此格没有植物");
            return;
        }
        const auto type = plant->GetPlantType();
        int imitater = -1;
        if (const auto imitaterType = plant->GetImitaterType(); imitaterType)
            imitater = int(imitaterType.Value().Value());
        const std::string name = PlantDisplayName(type ? int(type.Value().Value()) : -1, imitater);
        // 卖植物：先查售价再走原生出售（UseToolOnPlant，宿主植物条目同款分支）。
        if (tool.cursor == int(tao::enums::CursorType::CURSOR_TYPE_MONEY_SIGN.Value())) {
            int price = -1;
            if (const auto value = garden->GetPlantSellPrice(plant)) price = value.Value();
            if (garden->UseToolOnPlant(plant, tao::CursorType{tool.cursor}))
                Say(price > 0 ? "已出售" + name + "，获得 " + std::to_string(price) + " 金币"
                              : "已出售" + name);
            else
                Say("现在不能出售" + name);
            return;
        }
        // 手套：拿起植物进入搬移态，句柄由 mod 记住供落格使用。
        if (tool.cursor == int(tao::enums::CursorType::CURSOR_TYPE_GLOVE.Value())) {
            if (garden->UseToolOnPlant(plant, tao::CursorType{tool.cursor})) {
                gardenCarried = plant.Handle();
                Say("已拿起" + name + "，方向键移动，回车放下");
            } else {
                Say("现在不能拿起" + name);
            }
            return;
        }
        // 手推车：把植物装车（已装车与动态内容植物除外，宿主条目同款门禁）。
        if (tool.cursor == int(tao::enums::CursorType::CURSOR_TYPE_WHEEELBARROW.Value())) {
            const auto barrow = garden->GetPottedPlantInWheelbarrow();
            if (barrow && !barrow.Value().Empty()) { Say("手推车上已经有植物"); return; }
            const auto dynamic = plant->IsDynamicContent();
            if (dynamic && dynamic.Value()) { Say(name + "不能装上手推车"); return; }
            if (garden->UseToolOnPlant(plant, tao::CursorType{tool.cursor})) {
                gardenCarried = {};
                Say("已把" + name + "装上手推车，回车放到目标格");
            } else {
                Say("现在不能装上手推车");
            }
            return;
        }
        // 浇灌类工具（FeedPlant，坐标与鼠标路径一致）：巧克力走 PlantCanHaveChocolate
        // 门禁，其余工具先核对需求——需求不符时播报它真正需要什么、不施用。
        const int need = GardenNeed(plant);
        if (tool.cursor == int(tao::enums::CursorType::CURSOR_TYPE_CHOCOLATE.Value())) {
            const auto can = garden->PlantCanHaveChocolate(plant);
            if (can && !can.Value()) { Say(name + "现在不能吃巧克力"); return; }
            const auto px = plant->GetX();
            const auto py = plant->GetY();
            if (!px || !py || !garden->FeedPlant(plant, px.Value() + 40, py.Value() + 40,
                    tao::CursorType{tool.cursor}, false)) {
                Say("现在不能给" + name + "喂巧克力");
                return;
            }
            Say("已给" + name + "喂巧克力");
            return;
        }
        // 浇水壶/肥料/杀虫剂/留声机的光标值 9..12 与需求值 1..4 恰好错 8。
        const int expected = tool.cursor - int(tao::enums::CursorType::CURSOR_TYPE_WATERING_CAN.Value()) +
            int(tao::enums::PottedPlantNeed::PLANTNEED_WATER.Value());
        const char* verb = GardenNeedName(expected);
        if (need != expected) {
            const char* needName = GardenNeedName(need);
            Say(needName ? name + "需要的是" + needName : name + "现在不需要照料");
            return;
        }
        const auto px = plant->GetX();
        const auto py = plant->GetY();
        if (!px || !py || !garden->FeedPlant(plant, px.Value() + 40, py.Value() + 40,
                tao::CursorType{tool.cursor}, false)) {
            Say(std::string("现在不能对") + name + std::string("使用") + tool.name);
            return;
        }
        // 施用后播报下一个需求：浇水→施肥→杀虫的顺序链是花园的核心信息。
        const int after = GardenNeed(plant);
        if (const char* next = GardenNeedName(after))
            Say(std::string("已") + verb + "，接下来需要" + next);
        else
            Say(std::string("已") + verb + "，" + name + "很高兴");
    }
    // 放下搬移中的植物（手套搬移/手推车整盆）：目标格须为空（宿主落点条目
    // enabled 同款判定），MovePlant / PlaceWheelbarrowPlant 后 ClearCursor 收尾。
    void PlaceCarriedPlant() {
        tao::Plant existing;
        if (GardenPlantAtCursor(existing)) { Say("这里已有植物"); return; }
        tao::ZenGarden garden;
        if (!ResolveGarden(garden)) { Say("现在不能放置"); return; }
        if (Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_GLOVE.Value())) {
            if (!gardenCarried.object_id) { Say("没有正在搬移的植物"); return; }
            auto plantResult = objects->Wrap<tao::Plant>(gardenCarried);
            if (!plantResult || plantResult.Value().Empty()) {
                gardenCarried = {};
                Say("搬移的植物已失效");
                return;
            }
            const auto moved = garden->MovePlant(plantResult.Value(), column, row);
            if (objects && objects->Available() && currentBoard.object_id)
                if (auto b = objects->Wrap<tao::Board>(currentBoard)) b.Value()->ClearCursor();
            gardenCarried = {};
            Say(moved ? "已放下" : "这里不能放置");
            return;
        }
        const auto placed = garden->PlaceWheelbarrowPlant(column, row);
        Say(placed && placed.Value() ? "已放下" : "这里不能放置");
    }
    // Shift：花园总览——各需求数量与臭臭状态。
    void AnnounceGardenStatus() {
        tao::ZenGarden garden;
        if (!ResolveGarden(garden)) { Say("花园信息不可用"); return; }
        const char* gardenName = "禅境花园";
        if (const auto type = garden->GetGardenType())
            switch (int(type.Value().Value())) {
            case 1: gardenName = "蘑菇园"; break; // GARDEN_MUSHROOM
            case 3: gardenName = "水族园"; break; // GARDEN_AQUARIUM
            default: break;                       // 0 主花园；2 = 手推车搬运态
            }
        const auto context = Context();
        int counts[5] = {};
        int total = 0;
        if (objects && objects->Available() && currentBoard.object_id)
            if (auto boardResult = objects->Wrap<tao::Board>(currentBoard))
                for (int r = 0; r < context.rows; ++r)
                    for (int c = 0; c < context.columns; ++c) {
                        const auto plant = boardResult.Value()->GetTopPlantAt(c, r,
                            tao::enums::PlantPriority::TOPPLANT_ANY);
                        if (!plant || plant.Value().Empty()) continue;
                        ++total;
                        const int need = GardenNeed(plant.Value());
                        if (need >= 0 && need <= 4) ++counts[need];
                    }
        std::string text = std::string(gardenName) + "，共 " + std::to_string(total) + " 株植物";
        bool anyNeed = false;
        for (int need = 1; need <= 4; ++need)
            if (counts[need]) {
                anyNeed = true;
                text += std::string("，") + std::to_string(counts[need]) + " 株需要" + GardenNeedName(need);
            }
        if (total && !anyNeed) text += "，全部茁壮成长";
        const auto stinky = garden->GetStinky();
        if (stinky && !stinky.Value().Empty()) {
            const auto sleeping = garden->IsStinkySleeping();
            text += sleeping && sleeping.Value() ? "，臭臭在睡觉" : "，臭臭醒着";
        }
        Say(std::move(text));
    }
    // 数字键 0：前往下一个花园（宿主"下一花园"条目 activate 的原生终点）。
    // 成功后播报新花园名（切换结果对盲用户不可感知，属信息播报而非冗余确认）。
    void GotoNextGarden() {
        if (!objects || !objects->Available() || !currentBoard.object_id) { Say("现在不能切换花园"); return; }
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) { Say("现在不能切换花园"); return; }
        const auto usable = boardResult.Value()->CanUseGameObject(
            tao::GameObjectType{int(tao::enums::GameObjectType::OBJECT_TYPE_NEXT_GARDEN.Value())});
        if (!usable || !usable.Value()) { Say("没有更多花园"); return; }
        tao::ZenGarden garden;
        if (!ResolveGarden(garden)) { Say("现在不能切换花园"); return; }
        if (!garden->GotoNextGarden()) { Say("现在不能切换花园"); return; }
        const char* gardenName = "禅境花园";
        if (const auto type = garden->GetGardenType())
            switch (int(type.Value().Value())) {
            case 1: gardenName = "蘑菇园"; break; // GARDEN_MUSHROOM
            case 3: gardenName = "水族园"; break; // GARDEN_AQUARIUM
            default: break;                       // 0 主花园；2 = 手推车搬运态
            }
        Say(gardenName);
    }
    // 退格（花园内空手）：离开花园回主菜单。花园的"菜单"按钮原生就是返回
    // 主菜单（ActivateMenuButton 尾部对 mode 43 走 DoBackToMain），这里与宿主
    // a11y"菜单"按钮条目 activate 同一调用；ZenGarden::LeaveGarden 不是这条
    // 路（实测无效）。成功不播报（场景切换显而易见）；开局前 ~10 秒的菜单按钮
    // 解锁倒计时内静默失败，与原生按钮行为一致。
    void LeaveZenGarden() {
        if (!objects || !objects->Available() || !currentBoard.object_id) { Say("现在不能离开花园"); return; }
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult || !boardResult.Value()->ActivateMenuButton())
            Say("现在不能离开花园");
    }
    // Tab：花园内打开疯狂戴夫的商店（ZenGarden::OpenStore，花园商店按钮的原生
    // 终点；店内导航由 Store 的弹窗锚点自动接管）。
    void OpenGardenStore() {
        tao::ZenGarden garden;
        if (!ResolveGarden(garden) || !garden->OpenStore()) Say("打开商店失败");
    }

    // ===== 智慧树（GAMEMODE_TREE_OF_WISDOM=50）=====
    // 宿主无障碍层只暴露"树肥"（RefreshSeedPacketFromCursor + PickUpTool(
    // OBJECT_TYPE_TREE_FOOD)）与"智慧树"（Challenge::TreeOfWisdomFertilize）
    // 两个条目（ModAccessibilityUi.inl），mod 复刻同一原生链路。
    // Challenge 代理（随棋盘解析，失败不缓存）。
    bool ResolveChallenge(tao::Challenge& out) {
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const auto challenge = boardResult.Value()->GetChallenge();
        if (!challenge) return false;
        out = challenge.Value();
        return true;
    }
    // 回车：空手 = 拿起肥料（无库存时 PickUpTool 不改光标，按结果播报）；
    // 持肥 = 对树施肥并播报新高度（TreeOfWisdomCanFeed 门禁与宿主条目同款）。
    void UseTreeFood() {
        boardFocus = true;
        tao::Challenge challenge;
        if (!ResolveChallenge(challenge)) { Say("智慧树不可用"); return; }
        if (Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_TREE_FOOD.Value())) {
            const auto canFeed = challenge->TreeOfWisdomCanFeed();
            if (!canFeed || !canFeed.Value()) { Say("智慧树现在不需要施肥"); return; }
            if (!challenge->TreeOfWisdomFertilize()) { Say("现在不能施肥"); return; }
            std::string text = "已施肥";
            if (const auto size = challenge->TreeOfWisdomGetSize(); size)
                text += "，智慧树高 " + std::to_string(size.Value()) + " 英尺";
            Say(std::move(text));
            return;
        }
        if (!objects || !objects->Available() || !currentBoard.object_id) { Say("智慧树不可用"); return; }
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) { Say("智慧树不可用"); return; }
        const tao::Board board = boardResult.Value();
        board->RefreshSeedPacketFromCursor();
        board->PickUpTool(tao::GameObjectType{int(tao::enums::GameObjectType::OBJECT_TYPE_TREE_FOOD.Value())});
        if (Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_TREE_FOOD.Value()))
            Say("已拿起肥料，回车对智慧树施肥");
        else
            Say("没有肥料，请到商店购买");
    }
    // Shift/方向键：树高与下一步操作提示（智慧树没有格子可导航）。
    void AnnounceTreeStatus() {
        std::string text = "智慧树";
        tao::Challenge challenge;
        if (ResolveChallenge(challenge))
            if (const auto size = challenge->TreeOfWisdomGetSize(); size)
                text += "，高 " + std::to_string(size.Value()) + " 英尺";
        text += Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_TREE_FOOD.Value())
            ? "，回车施肥，退格放回肥料" : "，回车拿起肥料施肥，退格返回花园";
        Say(std::move(text));
    }
    // Tab/F7：打开商店买肥料（Challenge::TreeOfWisdomOpenStore，智慧树商店
    // 按钮的原生终点；店内导航由 Store 的弹窗锚点自动接管）。
    void OpenTreeStore() {
        tao::Challenge challenge;
        if (!ResolveChallenge(challenge) || !challenge->TreeOfWisdomOpenStore())
            Say("打开商店失败");
    }
    // 空手退格：返回禅境花园。原生终点是智慧树"下一花园"箭头的
    // Challenge::TreeOfWisdomNextGarden（棋盘就地拆建、模式回 43）；
    // Challenge::TreeOfWisdomLeave 是菜单按钮链路（DoBackToMain 回主菜单），
    // 仅作兜底。调用后核对模式，防止假成功/读数失败时连锁误退。
    void LeaveTreeOfWisdom() {
        tao::Challenge challenge;
        if (!ResolveChallenge(challenge)) { Say("现在不能返回花园"); return; }
        const int gardenMode = int(tao::enums::GameMode::GAMEMODE_CHALLENGE_ZEN_GARDEN.Value());
        challenge->TreeOfWisdomNextGarden();
        const int mode = CurrentGameMode();
        if (mode == gardenMode) {
            const char* gardenName = "禅境花园";
            tao::ZenGarden garden;
            if (ResolveGarden(garden))
                if (const auto type = garden->GetGardenType())
                    switch (int(type.Value().Value())) {
                    case 1: gardenName = "蘑菇园"; break; // GARDEN_MUSHROOM
                    case 3: gardenName = "水族园"; break; // GARDEN_AQUARIUM
                    default: break;                       // 0 主花园
                    }
            Say(std::string("已返回") + gardenName);
            return;
        }
        if (mode != int(tao::enums::GameMode::GAMEMODE_TREE_OF_WISDOM.Value()))
            return; // 已离开智慧树或读数失败：不妄动，由下次按键重新判定
        if (!challenge->TreeOfWisdomLeave())
            Say("现在不能返回花园");
    }

    void SelectShovel() {
        // 铲子合法性门禁（PDB 反汇编 20261005 exe，与宿主绘制铲槽条件一致）：
        // mShowShovel（GetShowShovel）且 !ShouldHideShovelAndMenuUI()
        //（后者 = IsIZombieLevel||IsScaryPotterLevel，我是僵尸/砸罐关禁铲）。
        // 语义 SELECT_SHOVEL 兜底无任何门禁、会强行拿铲，必须在入口拦下。
        // 禅境花园没有铲子：铲掉盆栽等于直接丢弃玩家收集的植物，拒绝并指引用卖植物标签。
        if (IsZenGarden()) { Say("花园里不能用铲子，用数字键 7 卖植物"); return; }
        const bool shovelAllowed = ShovelAllowed();
        if (!shovelAllowed) { Say("本关不能使用铲子"); return; }
        // 持卡时先语义 CANCEL 正常放回：宿主 SELECT_SHOVEL 会强制收回手上卡包，
        // 但隐式收回走 WasPlanted 路径，把该卡重置进冷却（实机确认：被收回的卡
        // 此后 SELECT_SEED 永远报 COOLDOWN、原生点击也点不动，即"锁死"）。
        const int cursorNow = Context().cursor;
        if (cursorNow != int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value()) &&
            cursorNow != int(tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value())) {
            TaoPluginBoardActionResultV2 cancelResult{};
            InvokeRaw(TAO_BOARD_CANCEL, {}, cancelResult);
        }
        // 优先原生点击铲子槽（与鼠标同一入口），语义 SELECT_SHOVEL 仅作兜底。
        if (NativeSelectShovel()) {
            boardFocus = true;
            layer = 0;
            AnnounceLayer();
            return;
        }
        if (Invoke(TAO_BOARD_SELECT_SHOVEL, {}, false)) {
            boardFocus = true;
            layer = 0;
            AnnounceLayer();
            return;
        }
        LogShovelGate("选铲失败"); // 临时诊断
        Say("现在不能拾取铲子");
    }
    void CycleLayer() {
        ++layer;
        AnnounceLayer();
    }
    void ShovelCurrent() {
        TaoPluginShovelTargetV1 target{};
        if (!ShovelTarget(target)) { Say("此格没有可铲目标"); return; }
        if (Invoke(TAO_BOARD_SHOVEL, target.plant)) return;
        // 教学阶段持铲后宿主动作仍可能被拒，同样退到格子中心原生点击。
        if (NativeDigCell()) { Say("操作完成"); return; }
        LogShovelGate("铲植物失败"); // 临时诊断（Invoke 已播报原因）
    }
    // 两段确认：空手在炮上激活；瞄准时在当前格发射。返回 false 才走种植。
    bool TryCannonConfirm() {
        const auto context = Context();
        if (context.cursor == int(tao::enums::CursorType::CURSOR_TYPE_COBCANNON_TARGET.Value())) {
            FireSelectedCannon();
            return true;
        }
        if (context.cursor != int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value()) ||
            !objects || !objects->Available()) return false;
        auto b = objects->Wrap<tao::Board>(currentBoard);
        if (!b) return false;
        const auto top = b.Value()->GetTopPlantAt(column, row,
            tao::enums::PlantPriority::TOPPLANT_ONLY_NORMAL_POSITION);
        if (!top || top.Value().Empty() || !IsCannon(top.Value())) return false;
        if (!CannonReady(top.Value())) { Say("玉米炮尚未就绪或当前不可操作"); return true; }
        if (!CannonGate(b.Value())) { Say("当前不能操作玉米炮"); return true; }
        if (SelectCannon(b.Value(), top.Value()))
            Say("已进入炮击瞄准，移动到目标格后回车发射，退格取消");
        else Say("未能激活玉米炮");
        return true;
    }

    // C：按行、列选择第一门可激活的就绪炮，目标始终是按键时的格子。
    // 只在按键时扫描，不在每 tick 增加全场枚举。
    void FireReadyCannon() {
        const auto context = Context();
        if (!Gameplay(context) || IsZenGarden() || IsTreeOfWisdom() || HasSpecialBank()) {
            Say("此模式不支持玉米炮"); return;
        }
        if (context.cursor == int(tao::enums::CursorType::CURSOR_TYPE_COBCANNON_TARGET.Value())) {
            FireSelectedCannon(); return;
        }
        if (!objects || !objects->Available()) { Say("无法查询场上的玉米炮"); return; }
        auto b = objects->Wrap<tao::Board>(currentBoard);
        if (!b || !CannonGate(b.Value())) { Say("当前不能操作玉米炮"); return; }
        struct Candidate { tao::Plant plant; int row, column; };
        std::vector<Candidate> ready;
        ForEachViewObject("plants", [&](TaoPluginObjectHandle handle) {
            auto p = objects->Wrap<tao::Plant>(handle);
            if (!p || !CannonReady(p.Value())) return;
            const auto r = p.Value()->GetRow();
            const auto c = p.Value()->GetColumn();
            if (r && c && c.Value() >= 0 && c.Value() < context.columns &&
                r.Value() >= gridRowMin && r.Value() < gridRowMin + context.rows)
                ready.push_back({p.Value(), r.Value(), c.Value()});
        });
        std::sort(ready.begin(), ready.end(), [](const Candidate& a, const Candidate& b) {
            return a.row != b.row ? a.row < b.row : a.column < b.column;
        });
        for (const auto& candidate : ready) {
            if (!SelectCannon(b.Value(), candidate.plant)) continue;
            boardFocus = true;
            FireSelectedCannon();
            return;
        }
        Say("没有可发射的玉米炮");
    }

    // 特征契约：PlantState 35..38 = 装填/充能/就绪/发射，37 为就绪。
    // 动态植物 GetPlantType() 为 PLANT_NONE，不能靠名字或 SeedType==47 识别。
    // 原版类型只兜底尚未进入专用状态的初始化帧；复用此状态机的 mod 无需列白名单。
    static bool IsCannonState(int state) { return state >= 35 && state <= 38; }
    static bool IsCannon(const tao::Plant& plant) {
        const auto state = plant->GetState();
        if (state && IsCannonState(int(state.Value().Value()))) return true;
        const auto type = plant->GetPlantType();
        return type && type.Value() == tao::enums::PlantType::PLANT_COBCANNON;
    }
    static bool CannonReady(const tao::Plant& plant) {
        const auto state = plant->GetState();
        if (!state || state.Value().Value() != 37) return false;
        const auto dead = plant->GetDead();
        const auto squished = plant->GetSquished();
        const auto asleep = plant->GetAsleep();
        const auto onBoard = plant->GetOnBoard();
        const auto bungee = plant->GetBungeeState();
        return dead && !dead.Value() && squished && !squished.Value() &&
            asleep && !asleep.Value() && onBoard && onBoard.Value() &&
            bungee && bungee.Value().Value() == 0;
    }
    bool CannonGate(const tao::Board& board) {
        if (!Gameplay(Context())) return false;
        const auto allowed = board->CanInteractWithBoardButtons(true);
        const auto paused = board->GetPaused();
        const auto stopped = board->GetTimeStopCounter();
        return allowed && allowed.Value() && paused && !paused.Value() &&
            stopped && stopped.Value() == 0;
    }
    bool SelectCannon(const tao::Board& board, const tao::Plant& plant) {
        // 原生 MouseDown 让植物自己的逻辑（含 mod hook）决定能否激活。
        // 宿主 SELECT_CANNON 写死 mSeedType==47，故不走该分支。
        // GetPlantRect 覆盖真实植物范围，包含两格炮；不把选炮位置写进格子光标。
        if (!CannonReady(plant)) return false;
        const auto rect = plant->GetPlantRect();
        if (!rect || rect.Value().width <= 0 || rect.Value().height <= 0) return false;
        if (!board->RefreshSeedPacketFromCursor() || !board->ClearCursor()) return false;
        const auto bounds = rect.Value();
        if (!plant->MouseDown(int(bounds.x + bounds.width / 2), int(bounds.y + bounds.height / 2), 1))
            return false;
        // void 调用成功不代表真的选中了炮，以真实瞄准光标闭环确认。
        return Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_COBCANNON_TARGET.Value());
    }
    void FireSelectedCannon() {
        TaoPluginBoardActionResultV2 result{};
        if (InvokeRaw(TAO_BOARD_FIRE_CANNON, {}, result)) {
            boardFocus = true;
            Say(std::string("已发射，目标 ") + char('A' + row) + std::to_string(column + 1));
        } else {
            // 失败保留瞄准，让玩家重试/取消；不把失效选择解释成种植。
            Say(result.reason == TAO_ACTION_NO_SELECTION ? "所选玉米炮已不可发射，请退格取消瞄准" :
                ReasonText(result.reason));
        }
    }

    // 放回手上持有的卡片或工具（退格键）。卡片走语义 CANCEL（宿主
    // RefreshSeedPacketFromCursor 会把选中的种子包/掉落卡放回原处）；
    // 铲子须原生点击铲子槽，炮击瞄准须 ClearCursor。卡片/工具放回成功不播报。
    void ReturnHeldItem() {
        boardFocus = true;
        const int cursor = Context().cursor;
        if (cursor == int(tao::enums::CursorType::CURSOR_TYPE_COBCANNON_TARGET.Value())) {
            if (objects && objects->Available())
                if (auto b = objects->Wrap<tao::Board>(currentBoard)) b.Value()->ClearCursor();
            Say(Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value()) ?
                "已取消炮击瞄准" : "现在不能取消瞄准");
            return;
        }
        // 禅境花园：放回手上的工具或搬移中的植物（原生 ClearCursor，宿主植物
        // 条目 activate 的收尾同款路径）；空手时退格 = 离开花园回主菜单。
        // 放回成功不播报（显而易见的放回）。
        if (IsZenGarden()) {
            const bool carrying =
                cursor == int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_GLOVE.Value()) ||
                cursor == int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_WHEEL_BARROW.Value());
            const bool tool = cursor >= int(tao::enums::CursorType::CURSOR_TYPE_WATERING_CAN.Value()) &&
                cursor <= int(tao::enums::CursorType::CURSOR_TYPE_WHEEELBARROW.Value());
            if (carrying || tool) {
                gardenCarried = {};
                if (objects && objects->Available() && currentBoard.object_id)
                    if (auto b = objects->Wrap<tao::Board>(currentBoard))
                        b.Value()->ClearCursor();
                if (Context().cursor != int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value()))
                    Say("现在不能放回");
                return;
            }
            LeaveZenGarden();
            return;
        }
        // 智慧树：退格放回肥料（TREE_FOOD=17 不在花园工具 9..16 区间）；
        // 空手时返回禅境花园（TreeOfWisdomLeave）。
        if (IsTreeOfWisdom()) {
            if (cursor == int(tao::enums::CursorType::CURSOR_TYPE_TREE_FOOD.Value())) {
                if (objects && objects->Available() && currentBoard.object_id)
                    if (auto b = objects->Wrap<tao::Board>(currentBoard))
                        b.Value()->ClearCursor();
                if (Context().cursor != int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value()))
                    Say("现在不能放回");
                return;
            }
            LeaveTreeOfWisdom();
            return;
        }
        const bool card =
            cursor >= int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_BANK.Value()) &&
            cursor <= int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_WHEEL_BARROW.Value());
        if (cursor == int(tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value())) {
            if (!NativeReturnShovel()) Say("现在不能放回铲子");
            return;
        }
        if (card) {
            TaoPluginBoardActionResultV2 result{};
            if (!InvokeRaw(TAO_BOARD_CANCEL, {}, result)) Say(ReasonText(result.reason));
            return;
        }
        // 打僵尸关卡的锤子常驻手上、等同普通关卡的空光标，无处可放。
        if (cursor == int(tao::enums::CursorType::CURSOR_TYPE_HAMMER.Value())) { Say("锤子无法放下"); return; }
        Say("手上没有卡片或工具");
    }

    // 铲子可用性：mShowShovel（GetShowShovel）且非我是僵尸关。PDB 反汇编
    // （20261005 exe）结论：mShowShovel=1 的置位函数（0x1408AD000）只排除
    // 锤击(30)/保龄球(17)/BEGHOULED(20)/LAST_STAND(31)/WAR_AND_PEAS_2(32)/
    // STORMY_NIGHT(47)/TREE_OF_WISDOM(50)/首次冒险前几关——砸罐子(51..60)与
    // 我是僵尸(61..70)都是 mShowShovel=1，宿主 a11y 层也只按 mShowShovel 暴露
    // 铲子项（ModAccessibilityUi.inl:343），砸罐子关允许铲子（铲砸出的误植植物）。
    // 我是僵尸没有植物可铲，显式排除。注意不能用 ShouldHideShovelAndMenuUI()
    // 当门禁：它=IsIZombieLevel||IsScaryPotterLevel，会把砸罐子一起错杀
    //（它只用于 Board::DrawShovel 的视觉隐藏，砸罐子关铲槽由挑战界面自绘）。
    bool ShovelAllowed() {
        if (IsIZombieLevel()) return false;
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const tao::Board board = boardResult.Value();
        const auto shown = board->GetShowShovel();
        return shown && shown.Value();
    }

    // 原生点击兜底：与鼠标操作同一入口（Board::MouseDown），
    // 坐标为棋盘局部坐标，参照 Coin::MouseDown 的用法。
    bool NativeSelectShovel() {
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const tao::Board board = boardResult.Value();
        if (const auto shown = board->GetShowShovel(); !shown || !shown.Value()) return false;
        const auto rect = board->GetShovelButtonRect();
        if (!rect) return false;
        const auto r = rect.Value();
        if (!board->MouseDown(int(r.x + r.width / 2), int(r.y + r.height / 2), 1)) return false;
        return Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value());
    }
    // 原生放回铲子：与鼠标再点一次铲子槽同一入口（原版持铲点铲子槽 = 放回）。
    // 语义 CANCEL 对铲子只改 a11y 上报、不放回真实光标，必须走这条原生路径。
    bool NativeReturnShovel() {
        if (Context().cursor != int(tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value())) return false;
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const tao::Board board = boardResult.Value();
        const auto rect = board->GetShovelButtonRect();
        if (!rect) return false;
        const auto r = rect.Value();
        if (!board->MouseDown(int(r.x + r.width / 2), int(r.y + r.height / 2), 1)) return false;
        const int after = Context().cursor;
        return after == int(tao::enums::CursorType::CURSOR_TYPE_NORMAL.Value());
    }
    // 原生点击选卡：与鼠标点卡同一入口。卡槽中心取 SeedPacket 对象实测矩形
    // （target_at 的目标句柄），矩形为棋盘局部坐标（与铲子槽矩形同一坐标系）。
    bool NativeSelectSeed(TaoPluginObjectHandle targetHandle) {
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const tao::Board board = boardResult.Value();
        auto packetResult = objects->Wrap<tao::SeedPacket>(targetHandle);
        if (!packetResult) return false;
        const auto packet = packetResult.Value();
        const auto px0 = packet->GetX();
        const auto py0 = packet->GetY();
        const auto pw = packet->GetWidth();
        const auto ph = packet->GetHeight();
        if (!px0 || !py0 || !pw || !ph || pw.Value() <= 0 || ph.Value() <= 0) return false;
        const int cx = px0.Value() + pw.Value() / 2;
        const int cy = py0.Value() + ph.Value() / 2;
        if (board->MouseDown(cx, cy, 1) &&
            Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_BANK.Value()))
            return true;
        // 棋盘坐标系不匹配时退到卡包自身的 mouse_down（窗口部件局部坐标）
        packet->MouseDown(pw.Value() / 2, ph.Value() / 2, 1);
        if (Context().cursor == int(tao::enums::CursorType::CURSOR_TYPE_PLANT_FROM_BANK.Value())) return true;
        return false;
    }
    bool NativeDigCell() {
        if (Context().cursor != int(tao::enums::CursorType::CURSOR_TYPE_SHOVEL.Value())) return false;
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const tao::Board board = boardResult.Value();
        const auto context = Context();
        const auto px = board->GridToPixelX(column, row);
        const auto py = board->GridToPixelY(column, row);
        if (!px || !py) return false;
        int halfW = 40, halfH = 50;
        if (column + 1 < context.columns)
            if (const auto next = board->GridToPixelX(column + 1, row); next && next.Value() > px.Value())
                halfW = (next.Value() - px.Value()) / 2;
        if (row + 1 < context.rows)
            if (const auto next = board->GridToPixelY(column, row + 1); next && next.Value() > py.Value())
                halfH = (next.Value() - py.Value()) / 2;
        TaoPluginShovelTargetV1 before{};
        if (!ShovelTarget(before)) return false;
        if (!board->MouseDown(px.Value() + halfW, py.Value() + halfH, 1)) return false;
        TaoPluginShovelTargetV1 after{};
        return !ShovelTarget(after); // 格子目标已被移除视为挖除成功
    }
    // 打开暂停菜单：直接调宿主导出 LawnApp::DoNewOptions(false)。
    // 旧实现调 Board::ActivateMenuButton()，PDB 反汇编（真身 0x140828BD0）确认其
    // 门禁极多且全部静默 return，正是"很多关卡 ESC 无效、空格暂停恢复后才偶尔
    // 有效"的根因：
    //   1) ShouldHideShovelAndMenuUI()（=IsIZombieLevel||IsScaryPotterLevel）为真
    //      直接 return —— 我是僵尸(61..70)/砸罐子(51..60)的菜单按钮被宿主隐藏，
    //      ActivateMenuButton 在这 20 关永远无效；
    //   2) CanInteractWithBoardButtons(false)（真身 0x14082A9B0）还要求：
//      a) [board+0x5F60] < 0：开局置 600/500 tick 的"菜单按钮解锁"倒计时，
//         未走完期间 ESC/菜单按钮全部无效（约前 10 秒）；
    //      b) mPaused（[board+0xED8]）== 0，已暂停时失败；
    //      c) mApp 虚表 +0x268 的已开对话框计数 <= 0；
    //      d) 手上光标类型必须为 0/9/10 —— 拿着植物/铲子/工具时失败；
    //      e) mChallenge 状态（[mChallenge+0x104]）!= 8；f) [mApp+0xAB4]==0。
    //   空格暂停绕道的解释：Space 走 CanPauseNow+DoPauseDialog，不吃上面 1)/2a)
    //   的门禁；暂停消耗掉开局倒计时窗口后，恢复再按 ESC 才撞上 2b) 之外的通过态。
    // 正常关卡 ActivateMenuButton 的尾部动作本来就是 DoNewOptions(false)（仅禅境
    // 花园等 43/50/71 模式改走 DoBackToMain）。DoNewOptions 真身（0x1407FF360）只
    // 处理"对话框已打开"的重入，没有玩法门禁；此处再以 CanPauseNow() 做前置，
    // 过场/戴夫对话等不可暂停时刻保持静默（与 Space 行为一致）。
    // 前奏复刻 ActivateMenuButton 的 UpdateCursor+ClearCursor：它开选项前总是清掉
    // 手上光标。若跳过（2026-10-08 回归），手持卡片/铲子时开选项后"重新开始"会
    // 在光标仍持有对象的状态下拆棋盘，宿主 abort（c0000409/5）。
    void OpenPauseMenu() {
        if (objects && objects->Available() && currentBoard.object_id) {
            if (auto b = objects->Wrap<tao::Board>(currentBoard)) {
                b.Value()->UpdateCursor();
                b.Value()->ClearCursor();
            }
        }
        void* app = TaoEngineGetApp();
        if (!app || !LawnApp_CanPauseNow(app)) return;
        LawnApp_DoNewOptions(app, false);
    }
    // 原生锤击兜底：持锤光标时对当前格中心做原生左键点击（Board::MouseDown，
    // 与鼠标同一入口；打僵尸关卡左键即锤击）。
    bool NativeWhackCell() {
        if (Context().cursor != int(tao::enums::CursorType::CURSOR_TYPE_HAMMER.Value())) return false;
        if (!objects || !objects->Available() || !currentBoard.object_id) return false;
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return false;
        const tao::Board board = boardResult.Value();
        const auto context = Context();
        const auto px = board->GridToPixelX(column, row);
        const auto py = board->GridToPixelY(column, row);
        if (!px || !py) return false;
        int halfW = 40, halfH = 50;
        if (column + 1 < context.columns)
            if (const auto next = board->GridToPixelX(column + 1, row); next && next.Value() > px.Value())
                halfW = (next.Value() - px.Value()) / 2;
        if (row + 1 < context.rows)
            if (const auto next = board->GridToPixelY(column, row + 1); next && next.Value() > py.Value())
                halfH = (next.Value() - py.Value()) / 2;
        if (!board->MouseDown(px.Value() + halfW, py.Value() + halfH, 1)) return false;
        return true;
    }

    // 临时诊断：铲子类动作失败时记录宿主门控各条件快照
    void LogShovelGate(const char* what) {
        if (!log) return;
        const auto context = Context();
        std::string text = std::string(what) + " scene=" + std::to_string(context.scene) +
            " flags=" + std::to_string(context.flags) + " cursor=" + std::to_string(context.cursor);
        if (objects && objects->Available() && currentBoard.object_id) {
            auto boardResult = objects->Wrap<tao::Board>(currentBoard);
            if (boardResult) {
                const tao::Board board = boardResult.Value();
                if (auto v = board->GetTutorialState()) text += " tutorial=" + std::to_string(int(v.Value().Value()));
                if (auto v = board->CanInteractWithBoardButtons(true)) text += " canButtons=" + std::to_string(v.Value() ? 1 : 0);
                if (auto v = board->GetTimeStopCounter()) text += " timestop=" + std::to_string(v.Value());
                text += std::string(" seedbank=") + (board->GetSeedBank() ? "1" : "0");
            }
        }
        log(text);
    }

    void ToggleFreeze() {
        TaoPluginSimulationInfoV1 info{};
        info.struct_size = sizeof(info);
        host->simulation.describe(host->simulation.host_context, &info);
        const auto status = host->simulation.set_frozen(host->simulation.host_context, currentBoard, info.frozen ? 0 : 1);
        Say(status == TAO_PLUGIN_OK ? (info.frozen ? "继续游戏" : "游戏逻辑已冻结") : "冻结被其他 Mod 或当前模式阻止");
    }
    bool IsLastStandLevel() {
        return CurrentGameMode() == int(tao::enums::GameMode::GAMEMODE_CHALLENGE_LAST_STAND.Value());
    }
    // TodStringTranslate 导出探测（与 ZombieTypeFromPacket 的同款模式，独立
    // 缓存：谁笑到最后可能先于我是僵尸用到）。
    bool LastStandTranslateReady() {
#ifdef _WIN32
        static int probed = -1;
        if (probed < 0)
            probed = GetModuleHandleW(nullptr) &&
                GetProcAddress(GetModuleHandleW(nullptr), kTodStringTranslateExport) ? 1 : 0;
        return probed == 1;
#else
        return true;
#endif
    }
    // 谁笑到最后（GAMEMODE_CHALLENGE_LAST_STAND=31）开局/每旗间隙：宿主把
    // 进攻按钮暴露为无障碍节点（Board 控件的子项，带 ACTIVATE 动作，攻击
    // 开始后会从树上消失）。此阶段宿主拒绝冻结，Tab 改为激活该按钮；找不
    // 到按钮（攻击已开始/非该模式）返回 false，调用方回退冻结切换。
    // 开局按钮键是 [START_ONSLAUGHT]（展开攻击），旗间间隙是
    // [CONTINUE_ONSLAUGHT]（译文不同），名称经 TodStringTranslate 取译文匹配。
    bool TryStartLastStandAttack() {
        if (!IsLastStandLevel()) return false;
        std::vector<std::string> names;
        if (LastStandTranslateReady()) {
            for (const char* key : {"[START_ONSLAUGHT]", "[CONTINUE_ONSLAUGHT]"}) {
                std::string translated = TodStringTranslate(key);
                if (!translated.empty() && translated != key) names.push_back(std::move(translated));
            }
        }
        if (names.empty()) names = {"展开攻击"}; // 无译文可用时的内置回退
        std::vector<TaoPluginAccessibleNodeV2> pending;
        size_t count = 0;
        if (host->widgets.root_count(host->widgets.host_context, &count) != TAO_PLUGIN_OK) return false;
        for (size_t i = 0; i < count && i < 1024; ++i) {
            TaoPluginAccessibleNodeV2 node{};
            if (host->widgets.root_at(host->widgets.host_context, i, &node.widget) == TAO_PLUGIN_OK) pending.push_back(node);
        }
        for (size_t i = 0; i < pending.size(); ++i) {
            TaoPluginAccessibleInfoV2 info{};
            info.struct_size = sizeof(info);
            if (host->ui.describe(host->ui.host_context, pending[i], &info) != TAO_PLUGIN_OK) continue;
            const std::string nodeName = CopyView(info.name);
            if ((info.actions & TAO_ACCESSIBLE_ACTION_ACTIVATE) &&
                std::find(names.begin(), names.end(), nodeName) != names.end()) {
                invokeguard::Guard invokeGuard; // 与 ActivateFocused 同款：防与栈上阻塞型激活重叠
                if (host->ui.invoke(host->ui.host_context, pending[i], TAO_ACCESSIBLE_ACTION_ACTIVATE, 0, {}) != TAO_PLUGIN_OK)
                    return false;
                Say(nodeName);
                return true;
            }
            for (size_t j = 0; j < info.child_count && pending.size() < 1024; ++j) {
                TaoPluginAccessibleNodeV2 child{};
                if (host->ui.child_at(host->ui.host_context, pending[i], j, &child) == TAO_PLUGIN_OK) pending.push_back(child);
            }
        }
        return false;
    }
    void StepOnce() {
        Say(host->simulation.step(host->simulation.host_context, currentBoard, 1) == TAO_PLUGIN_OK ? "推进一帧" : "请先冻结游戏");
    }
    void ReleaseFreeze() {
        if (currentBoard.object_id) host->simulation.set_frozen(host->simulation.host_context, currentBoard, 0);
    }

private:
    void Say(std::string text, bool interrupt = true) const { if (say) say(std::move(text), interrupt); }

    // 格子朗读：植物状态 植物名称；僵尸状态 僵尸名称；……；地形，行列
    // 例：武装 土豆地雷；冰冻 路障僵尸；草坪，C5
    std::string ComposeCellText() {
        if (!objects || !objects->Available() || !currentBoard.object_id) return {};
        auto boardResult = objects->Wrap<tao::Board>(currentBoard);
        if (!boardResult) return {};
        const tao::Board boardProxy = boardResult.Value();
        zenCompose = IsZenGarden(); // PlantSegment 按此追加花园需求朗读

        // 分层取植物：底座（睡莲/花盆）、普通位、飞行、南瓜。
        // 朗读规则：底座上有普通植物时不读底座（地形信息里已含水面/屋顶）；
        // 普通植物被南瓜保护时南瓜在其后朗读；空南瓜视为普通植物。
        auto underPlant = boardProxy->GetTopPlantAt(column, row, tao::enums::PlantPriority::TOPPLANT_ONLY_UNDER_PLANT);
        auto normalPlant = boardProxy->GetTopPlantAt(column, row, tao::enums::PlantPriority::TOPPLANT_ONLY_NORMAL_POSITION);
        auto flyingPlant = boardProxy->GetTopPlantAt(column, row, tao::enums::PlantPriority::TOPPLANT_ONLY_FLYING);
        auto pumpkinPlant = boardProxy->GetTopPlantAt(column, row, tao::enums::PlantPriority::TOPPLANT_ONLY_PUMPKIN);
        const bool hasNormal = normalPlant && !normalPlant.Value().Empty();
        const bool hasPumpkin = pumpkinPlant && !pumpkinPlant.Value().Empty();
        std::vector<std::string> segments;
        if (underPlant && !underPlant.Value().Empty() && !hasNormal)
            segments.push_back(PlantSegment(underPlant.Value()));
        if (hasNormal)
            segments.push_back(PlantSegment(normalPlant.Value()));
        else if (hasPumpkin)
            segments.push_back(PlantSegment(pumpkinPlant.Value()));
        if (flyingPlant && !flyingPlant.Value().Empty())
            segments.push_back(PlantSegment(flyingPlant.Value()));
        if (hasNormal && hasPumpkin)
            segments.push_back(PlantSegment(pumpkinPlant.Value()));
        AppendZombies(boardProxy, segments);

        // 已种上植物的路标格视为完成，不再提示路标（南瓜罩空位仍需提示）。
        if (!hasNormal)
            if (const std::string marker = ArtMarkerSegment(boardProxy); !marker.empty())
                segments.push_back(marker);

        const auto terrain = GridItemTerrain();
        std::string terrainText = terrain.empty() ? BaseTerrain(boardProxy) : terrain;
        // 冰道（冰车/雪橇车僵尸留下的冰面）覆盖原地形朗读，不再追加。
        if (const auto ice = boardProxy->IsIceAt(column, row); ice && ice.Value())
            terrainText = "冰道";
        std::string text;
        for (const auto& segment : segments) {
            if (segment.empty()) continue;
            if (!text.empty()) text += "；";
            text += segment;
        }
        if (!text.empty()) text += "；";
        text += terrainText;
        text += "，";
        text += char('A' + row);
        text += std::to_string(column + 1);
        return text;
    }

    // 路标格子（观星 22 / 坚果艺术锦标赛 36 / 向日葵艺术锦标赛 40）：
    // 这类关卡要求把指定植物种在发亮的路标格上摆出图案。
    // Challenge::GetArtChallengeSeed(col,row) 返回该格要求的种子类型，
    // 非路标格返回 SEED_NONE(-1)；SeedType 与 PlantType 编号一致，名称复用
    // PlantDisplayName。播报形如"阳桃路标"。
    std::string ArtMarkerSegment(const tao::Board& boardProxy) {
        const int mode = CurrentGameMode();
        if (mode != int(tao::enums::GameMode::GAMEMODE_CHALLENGE_SEEING_STARS.Value()) &&
            mode != int(tao::enums::GameMode::GAMEMODE_CHALLENGE_ART_CHALLENGE_WALLNUT.Value()) &&
            mode != int(tao::enums::GameMode::GAMEMODE_CHALLENGE_ART_CHALLENGE_SUNFLOWER.Value()))
            return {};
        const auto challenge = boardProxy->GetChallenge();
        if (!challenge) return {};
        const auto seed = challenge.Value()->GetArtChallengeSeed(column, row);
        if (!seed) return {};
        const int seedType = int(seed.Value().Value());
        if (seedType < 0) return {};
        return PlantDisplayName(seedType, -1) + "路标";
    }

    std::string PlantSegment(const tao::Plant& plant) {
        const auto type = plant->GetPlantType();
        if (!type) return {};
        const int plantType = int(type.Value().Value());
        std::string prefix;
        const auto state = plant->GetState();
        const int plantState = state ? int(state.Value().Value()) : 0;
        if (const auto asleep = plant->GetAsleep(); asleep && asleep.Value()) prefix = "睡着 ";
        else if (plantType == int(tao::enums::PlantType::PLANT_POTATOMINE.Value())) {
            if (plantState == 16) prefix = "武装 ";            // STATE_POTATO_ARMED
            else if (plantState == 0 || plantState == 15) prefix = "未出土 "; // NOTREADY / POTATO_RISING
        } else if (IsCannonState(plantState) || plantType == int(tao::enums::PlantType::PLANT_COBCANNON.Value())) {
            prefix = plantState == 37 ? "就绪 " : plantState == 38 ? "发射中 " : "充能 ";
        } else if (plantType == int(tao::enums::PlantType::PLANT_CHOMPER.Value())) {
            if (plantState >= 10 && plantState <= 14) prefix = "咀嚼中 ";
        } else if (plantType == int(tao::enums::PlantType::PLANT_SUNSHROOM.Value())) {
            if (plantState == 23 || plantState == 24) prefix = "小 ";
        } else if (plantType == int(tao::enums::PlantType::PLANT_MAGNETSHROOM.Value())) {
            // 吸附状态（Plant.h PlantState，tao::enums 不含该枚举，沿用裸值）：
            // 无前缀 = 可吸取；睡着（白天）优先级更高，睡着时不会进入这两个状态。
            if (plantState == 26) prefix = "吸取中 ";     // STATE_MAGNETSHROOM_SUCKING（吸取动画）
            else if (plantState == 27) prefix = "吸附中 "; // STATE_MAGNETSHROOM_CHARGING（吸着铁器，冷却中暂不能再吸）
        } else if (plantType == int(tao::enums::PlantType::PLANT_WALLNUT.Value()) ||
                   plantType == int(tao::enums::PlantType::PLANT_TALLNUT.Value()) ||
                   plantType == int(tao::enums::PlantType::PLANT_PUMPKINSHELL.Value())) {
            const auto health = plant->GetHealth();
            const auto maxHealth = plant->GetMaxHealth();
            if (health && maxHealth && maxHealth.Value() > 0) {
                if (health.Value() * 3 <= maxHealth.Value()) prefix = "损坏 ";
                else if (health.Value() * 3 <= maxHealth.Value() * 2) prefix = "破损 ";
            }
        }
        int imitater = -1;
        if (const auto imitaterType = plant->GetImitaterType(); imitaterType)
            imitater = int(imitaterType.Value().Value());
        std::string name = PlantDisplayName(plantType, imitater);
        // 禅境花园：随植物名朗读需求状态（数字键选工具、回车使用的操作提示）。
        if (zenCompose) {
            const int need = GardenNeed(plant);
            if (const char* needName = GardenNeedName(need))
                name += std::string("，需要") + needName;
            else if (need == int(tao::enums::PottedPlantNeed::PLANTNEED_NONE.Value()))
                name += "，无需照料";
        }
        return prefix + name;
    }

    // 僵尸状态前缀（格子朗读、Shift 行播报与循环查看共用）：
    // 魅惑/冰冻/黄油/巨人半血/红眼血档/举锤/投掷小鬼/撑杆持杆与跳跃/小鬼被投掷/
    // 断头/断手/防具磨损与暴露。
    // 断头与断手互斥：断头即濒死（死亡动画进行中），手臂状态无意义，断头优先、
    // else-if 保证只报其一。防具磨损覆盖头盔（路障/铁桶/橄榄球帽等，HelmHealth）
    // 与护盾（铁门/报纸/梯子，ShieldHealth）两条独立血量，各按剩余比例分级；
    // 前缀统一两字（与坚果/南瓜的"损坏/破损"同风格）：<1/3 "损坏"、<2/3 "破损"、
    // 归零后报"暴露"——防具被打掉或被磁力菇吸走，僵尸失去额外防护（读报僵尸
    // 例外：报纸破碎进入暴怒，报"愤怒"提示其加速冲刺）。
    // 僵尸名称本身已含防具种类（铁桶僵尸/铁门僵尸），前缀不再区分戴的还是举的。
    static bool ZombiePhaseIs(const tao::Zombie& z, int phase) {
        const auto value = z->GetPhase();
        return value && int(value.Value().Value()) == phase;
    }
    std::string ZombieStatusPrefix(const tao::Zombie& z) {
        std::string prefix;
        if (const auto mindless = z->GetMindControlled(); mindless && mindless.Value()) prefix += "魅惑 ";
        if (const auto chilled = z->GetChilledCounter(); chilled && chilled.Value() > 0) prefix += "冰冻 ";
        if (const auto buttered = z->GetButteredCounter(); buttered && buttered.Value() > 0) prefix += "黄油 ";
        // 巨人：血量降到一半以下报"半血"——游戏在此血线出现裂纹并达到投掷小鬼的
        // 血线。红眼巨人血量约为巨人两倍，改报剩余血档：每损失 1800 血降一档，
        // 大于满血-1800 为"3血"、大于满血-2*1800 为"2血"、大于满血-3*1800 为
        // "1血"、再低为"0血"（满血即 3 血，随消耗递减）。砸击动画期间
        //（PHASE_GARGANTUAR_SMASHING）报"举锤"，投掷动画期间
        //（PHASE_GARGANTUAR_THROWING）报"投掷小鬼"。
        // 撑杆僵尸：持杆冲刺（PHASE_POLEVAULTER_PRE_VAULT，尚未起跳、仍能跳过
        // 首株植物）报"持杆"，跳跃途中（PHASE_POLEVAULTER_IN_VAULT）报"跳跃"；
        // 跳完（POST_VAULT）与普通僵尸无异，不报。
        // 小鬼被抛出飞行期间（PHASE_IMP_GETTING_THROWN）报"被投掷"。
        const auto zombieType = z->GetZombieType();
        const int zombieTypeValue = zombieType ? int(zombieType.Value().Value()) : -1;
        const bool isGargantuar =
            zombieTypeValue == int(tao::enums::ZombieType::ZOMBIE_GARGANTUAR.Value()) ||
            zombieTypeValue == int(tao::enums::ZombieType::ZOMBIE_REDEYE_GARGANTUAR.Value());
        if (isGargantuar) {
            const auto bodyHealth = z->GetBodyHealth();
            const auto bodyMaxHealth = z->GetBodyMaxHealth();
            if (bodyHealth && bodyMaxHealth && bodyMaxHealth.Value() > 0) {
                if (zombieTypeValue == int(tao::enums::ZombieType::ZOMBIE_REDEYE_GARGANTUAR.Value())) {
                    const int damage = bodyMaxHealth.Value() - bodyHealth.Value();
                    int tier = 3 - damage / 1800;
                    if (tier < 0) tier = 0;
                    prefix += std::to_string(tier) + "血 ";
                } else if (bodyHealth.Value() * 2 <= bodyMaxHealth.Value()) {
                    prefix += "半血 ";
                }
            }
            if (ZombiePhaseIs(z, int(tao::enums::ZombiePhase::PHASE_GARGANTUAR_SMASHING.Value())))
                prefix += "举锤 ";
            if (ZombiePhaseIs(z, int(tao::enums::ZombiePhase::PHASE_GARGANTUAR_THROWING.Value())))
                prefix += "投掷小鬼 ";
        } else if (zombieTypeValue == int(tao::enums::ZombieType::ZOMBIE_IMP.Value())) {
            if (ZombiePhaseIs(z, int(tao::enums::ZombiePhase::PHASE_IMP_GETTING_THROWN.Value())))
                prefix += "被投掷 ";
        } else if (zombieTypeValue == int(tao::enums::ZombieType::ZOMBIE_POLEVAULTER.Value())) {
            if (ZombiePhaseIs(z, int(tao::enums::ZombiePhase::PHASE_POLEVAULTER_PRE_VAULT.Value())))
                prefix += "持杆 ";
            else if (ZombiePhaseIs(z, int(tao::enums::ZombiePhase::PHASE_POLEVAULTER_IN_VAULT.Value())))
                prefix += "跳跃 ";
        }
        if (const auto head = z->GetHasHead(); head && !head.Value()) prefix += "断头 ";
        else if (const auto arm = z->GetHasArm(); arm && !arm.Value()) prefix += "断手 ";
        AppendArmorWear(z->GetHelmHealth(), z->GetHelmMaxHealth(), prefix, true);
        // 护盾归零的"暴露"读报僵尸除外（报纸破碎走"愤怒"分支）。
        AppendArmorWear(z->GetShieldHealth(), z->GetShieldMaxHealth(), prefix,
            zombieTypeValue != int(tao::enums::ZombieType::ZOMBIE_NEWSPAPER.Value()));
        if (NewspaperAngered(z)) prefix += "愤怒 ";
        return prefix;
    }
    template <typename TResult>
    static void AppendArmorWear(const TResult& health, const TResult& maxHealth, std::string& prefix,
        bool reportExposed) {
        if (!health || !maxHealth || maxHealth.Value() <= 0) return;
        if (health.Value() <= 0) {
            if (reportExposed) prefix += "暴露 ";
            return;
        }
        if (health.Value() * 3 <= maxHealth.Value()) prefix += "损坏 ";
        else if (health.Value() * 3 <= maxHealth.Value() * 2) prefix += "破损 ";
    }
    // 读报僵尸（ZOMBIE_NEWSPAPER）暴怒：护盾类型仍是报纸（SHIELDTYPE_NEWSPAPER=1）
    // 但血量已归零——报纸被打掉、僵尸进入愤怒冲刺。其余护盾（梯子/铁门被打掉
    // 或被磁力菇吸走）由 AppendArmorWear 统一报"暴露"。
    static bool NewspaperAngered(const tao::Zombie& z) {
        const auto shieldType = z->GetShieldType();
        const auto shieldHealth = z->GetShieldHealth();
        if (!shieldType || !shieldHealth || shieldHealth.Value() > 0) return false;
        const auto zombieType = z->GetZombieType();
        return zombieType &&
            int(zombieType.Value().Value()) == int(tao::enums::ZombieType::ZOMBIE_NEWSPAPER.Value());
    }

    // ===== 僵王博士火球/冰球（网格浏览与 Shift 行播报共用）=====
    // 球挂在博士僵尸（ZOMBIE_BOSS）身上：mBossFireBallReanimID 非空即有球，
    // 行号取 mFireballRow、冰火取 mIsFireball；列需解 reanimation 位置
    //（偏移依据见 kReanimPosXOffset 注释）。
    struct BossBallInfo { bool active = false; bool isFire = false; int ballRow = -1; int ballCol = -1; };
    void CheckBossBall(const tao::Zombie& z, const tao::Board& boardProxy, int zombieType, BossBallInfo& info) {
        if (info.active || zombieType != int(tao::enums::ZombieType::ZOMBIE_BOSS.Value())) return;
        const auto reanimId = z->GetBossFireballReanimId();
        if (!reanimId || reanimId.Value().Value() == 0) return; // REANIMATIONID_NULL：当前没有球
        info.active = true;
        if (const auto ballRow = z->GetFireballRow(); ballRow) info.ballRow = int(ballRow.Value());
        if (const auto fire = z->GetIsFireball(); fire) info.isFire = fire.Value();
        info.ballCol = ResolveBossBallColumn(boardProxy, static_cast<uint64_t>(reanimId.Value().Value()));
    }
    int ResolveBossBallColumn(const tao::Board& boardProxy, uint64_t reanimId) {
        // 直接调宿主导出 LawnApp::ReanimationTryToGet 取 Reanimation*（id 失效返回空）。
        void* app = TaoEngineGetApp();
        void* reanim = app ? LawnApp_ReanimationTryToGet(app, static_cast<uint32_t>(reanimId)) : nullptr;
        if (!reanim) { LogBossBallResolve("reanim 解析失败"); return -1; }
        const float ballX = *reinterpret_cast<const float*>(static_cast<const char*>(reanim) + kReanimPosXOffset);
        const float ballY = *reinterpret_cast<const float*>(static_cast<const char*>(reanim) + kReanimPosYOffset);
        if (!std::isfinite(ballX) || !std::isfinite(ballY) ||
            ballX < -400.0f || ballX > 1200.0f || ballY < -200.0f || ballY > 800.0f) {
            LogBossBallResolve("坐标读数越界");
            return -1;
        }
        const auto col = boardProxy->PixelToGridXKeepOnBoard(int(ballX), int(ballY));
        LogBossBallResolve("x=" + std::to_string(ballX) + " y=" + std::to_string(ballY) +
            (col ? " 列=" + std::to_string(col.Value()) : " 换算失败"));
        return col ? col.Value() : -1;
    }
    void LogBossBallResolve(const std::string& outcome) {
        if (!log || bossBallResolveLogged) return;
        bossBallResolveLogged = true;
        log("僵王球位置诊断：" + outcome);
    }

    void AppendZombies(const tao::Board& boardProxy, std::vector<std::string>& segments) {
        std::string view;
        if (!ViewName('z', view)) return;
        BossBallInfo bossBall;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto zombie = objects->Wrap<tao::Zombie>(handle);
            if (!zombie) return;
            const auto z = zombie.Value();
            if (const auto dead = z->GetDead(); !dead || dead.Value()) return;
            const auto type = z->GetZombieType();
            if (!type) return;
            const int zombieType = int(type.Value().Value());
            CheckBossBall(z, boardProxy, zombieType, bossBall);
            const auto zRow = z->GetRow();
            const auto posX = z->GetPosX();
            const auto posY = z->GetPosY();
            if (!zRow || !posX || !posY || zRow.Value() != row) return;
            // 与 PvZA11y 一致：僵尸判定格 = (posX + 75) 换算列
            const auto zCol = boardProxy->PixelToGridXKeepOnBoard(int(posX.Value() + 75.0f), int(posY.Value()));
            if (!zCol || zCol.Value() != column) return;
            segments.push_back(ZombieStatusPrefix(z) + ZombieDisplayName(zombieType, zombieShortNames));
        });
        // 僵王球：列已知只在对应格朗读；列未知（位置解析失败）时整行朗读保证可发现。
        if (bossBall.active && bossBall.ballRow == row &&
            (bossBall.ballCol == column || bossBall.ballCol < 0))
            segments.push_back(bossBall.isFire ? "僵王火球" : "僵王冰球");
    }

    std::string GridItemTerrain() {
        std::string view;
        if (!ViewName('g', view)) return {};
        std::string terrain;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            if (!terrain.empty()) return;
            auto item = objects->Wrap<tao::GridItem>(handle);
            if (!item) return;
            const auto gridItem = item.Value();
            if (const auto dead = gridItem->GetDead(); !dead || dead.Value()) return;
            const auto gridX = gridItem->GetGridX();
            const auto gridY = gridItem->GetGridY();
            const auto type = gridItem->GetType();
            if (!gridX || !gridY || !type) return;
            if (gridX.Value() != column || gridY.Value() != row) return;
            switch (int(type.Value().Value())) {
            case 1: terrain = "墓碑"; break;
            case 2: terrain = "弹坑"; break;
            case 3: terrain = "梯子"; break;
            case 4: terrain = "圆形传送门"; break;
            case 5: terrain = "方形传送门"; break;
            case 6: case 12: terrain = "脑子"; break;
            case 7: terrain = VaseName(gridItem); break;
            case 11: terrain = "钉耙"; break;
            default: break;
            }
        });
        return terrain;
    }

    std::string VaseName(const tao::GridItem& vase) {
        // 透明计数是游戏的实际透视状态：路灯花照明和工具箱透视均走此字段。
        // 未透视时只读外观标记，不能用隐藏内容推断普通问号花瓶的种类。
        const auto transparent = vase->GetTransparentCounter();
        if (transparent && transparent.Value() > 0) {
            const auto content = vase->GetScaryPotType();
            if (content) {
                switch (int(content.Value().Value())) {
                case 1: {
                    const auto seed = vase->GetSeedType();
                    if (seed) return PlantDisplayName(int(seed.Value().Value()), -1) + "花瓶";
                    break;
                }
                case 2: {
                    const auto zombie = vase->GetZombieType();
                    if (zombie) return ZombieDisplayName(int(zombie.Value().Value()), zombieShortNames) + "花瓶";
                    break;
                }
                case 3: return "阳光花瓶";
                default: break;
                }
            }
        }
        const auto state = vase->GetState();
        if (state) {
            switch (int(state.Value().Value())) {
            case 4: return "植物花瓶"; // GRIDITEM_STATE_SCARY_POT_LEAF
            case 5: return "僵尸花瓶"; // GRIDITEM_STATE_SCARY_POT_ZOMBIE
            default: break;
            }
        }
        return "花瓶";
    }

    std::string BaseTerrain(const tao::Board& boardProxy) const {
        const auto background = boardProxy->GetBackground();
        if (!background) return "草坪";
        switch (int(background.Value().Value())) {
        case 2: case 3: return row == 2 || row == 3 ? "泳池" : "草坪"; // 泳池/浓雾
        case 4: case 5: return column <= 4 ? "斜顶" : "平顶";           // 屋顶/僵王
        case 6: case 7: return "花园";
        case 8: return "水族馆";
        default: return "草坪";
        }
    }

    // 视图名由宿主公开；打开失败时枚举棋盘字段寻找实际名称并记录诊断，只解析一次。
    bool ViewName(char key, std::string& out) {
        ResolveViews();
        out = key == 'z' ? zombiesView : key == 'c' ? coinsView : key == 'm' ? mowersView
            : key == 'p' ? seedPacketsView : gridItemsView;
        return !out.empty();
    }
    void ResolveViews() {
        if (resolvedViews || !currentBoard.object_id) return;
        resolvedViews = true;
        zombiesView = TestView("zombies") ? "zombies" : "";
        gridItemsView = TestView("grid_items") ? "grid_items" : "";
        coinsView = TestView("coins") ? "coins" : "";
        mowersView = TestView("lawn_mowers") ? "lawn_mowers" : TestView("mowers") ? "mowers" : "";
        seedPacketsView = TestView("seed_packets") ? "seed_packets" : "";
        std::string candidates, catalog;
        size_t fieldCount = 0;
        if (host->objectsApi.field_count &&
            host->objectsApi.field_count(host->objectsApi.host_context, currentBoard, &fieldCount) == TAO_PLUGIN_OK) {
            for (size_t i = 0; i < fieldCount; ++i) {
                TaoPluginFieldDescriptionV1 field{};
                field.struct_size = sizeof(field);
                if (host->objectsApi.field_at(host->objectsApi.host_context, currentBoard, i, &field) != TAO_PLUGIN_OK) break;
                const auto access = CopyView(field.access_name);
                const auto name = CopyView(field.name);
                catalog += " " + name + "/" + access;
                if (access != "VIEW") continue;
                candidates += " " + name;
                if (zombiesView.empty() && name.find("zombie") != std::string::npos && TestView(name)) zombiesView = name;
                if (gridItemsView.empty() && name.find("grid") != std::string::npos && TestView(name)) gridItemsView = name;
                if (coinsView.empty() && name.find("coin") != std::string::npos && TestView(name)) coinsView = name;
                if (mowersView.empty() && name.find("mower") != std::string::npos && TestView(name)) mowersView = name;
                if (seedPacketsView.empty() && name.find("packet") != std::string::npos && TestView(name)) seedPacketsView = name;
            }
        }
        if (log && !catalog.empty()) log("棋盘字段目录：" + catalog); // 临时诊断：确认出怪字段是否暴露
        if (log && !candidates.empty()) log("可用视图字段：" + candidates);
    }
    bool TestView(const std::string& name) {
        if (!host->views.open || !host->views.close) return false;
        TaoPluginViewHandle view{};
        view.struct_size = sizeof(view);
        if (host->views.open(host->views.host_context, currentBoard, tao::AbiString(name), 0, &view) != TAO_PLUGIN_OK)
            return false;
        host->views.close(host->views.host_context, view);
        return true;
    }
    void ForEachViewObject(const std::string& name, const std::function<void(TaoPluginObjectHandle)>& visit) {
        if (!host->views.open || !host->views.close || !host->views.size || !host->views.get) return;
        TaoPluginViewHandle view{};
        view.struct_size = sizeof(view);
        if (host->views.open(host->views.host_context, currentBoard, tao::AbiString(name), 0, &view) != TAO_PLUGIN_OK)
            return;
        size_t count = 0;
        host->views.size(host->views.host_context, view, &count);
        for (size_t i = 0; i < count && i < 512; ++i) {
            TaoPluginValue value{};
            value.struct_size = sizeof(value);
            if (host->views.get(host->views.host_context, view, i, &value) != TAO_PLUGIN_OK) break;
            if (value.kind == TAO_VALUE_OBJECT && value.value.object_value.object_id)
                visit(value.value.object_value);
        }
        host->views.close(host->views.host_context, view);
    }

    // 名称解析：优先运行时取游戏自身的本地化名称，取不到（接口缺失或返回
    // 未本地化的字符串 ID，如 "ZOMBIE_FLAG"）时回退内置中文表。
    static bool LooksLocalized(const std::string& text) {
        return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x80; });
    }
    static const char* PlantNameFallback(int type) {
        static const char* names[] = {
            "豌豆射手", "向日葵", "樱桃炸弹", "坚果墙", "土豆地雷", "寒冰射手", "大嘴花", "双发射手",
            "小喷菇", "阳光菇", "大喷菇", "墓碑吞噬者", "魅惑菇", "胆小菇", "寒冰菇", "毁灭菇",
            "睡莲", "窝瓜", "三线射手", "缠绕海草", "火爆辣椒", "地刺", "火炬树桩", "高坚果",
            "海蘑菇", "路灯花", "仙人掌", "三叶草", "裂荚射手", "杨桃", "南瓜头", "磁力菇",
            "卷心菜投手", "花盆", "玉米投手", "咖啡豆", "大蒜", "叶子保护伞", "金盏花", "西瓜投手",
            "机枪射手", "双子向日葵", "忧郁蘑菇", "香蒲", "冰瓜投手", "吸金磁", "地刺王", "玉米加农炮",
            "模仿者", "爆炸坚果", "巨大坚果", "幼苗", "反向双发射手"};
        return type >= 0 && type < int(std::size(names)) ? names[type] : "未知植物";
    }
    static const char* ZombieNameFallback(int type) {
        static const char* names[] = {
            "普通僵尸", "旗帜僵尸", "路障僵尸", "撑杆僵尸", "铁桶僵尸", "读报僵尸", "铁门僵尸", "橄榄球僵尸",
            "舞王僵尸", "伴舞僵尸", "鸭子救生圈僵尸", "潜水僵尸", "冰车僵尸", "雪橇车僵尸", "海豚骑士僵尸", "小丑僵尸",
            "气球僵尸", "矿工僵尸", "跳跳僵尸", "雪人僵尸", "蹦极僵尸", "扶梯僵尸", "投石车僵尸", "巨人僵尸",
            "小鬼僵尸", "僵王博士", "豌豆僵尸", "坚果僵尸", "辣椒僵尸", "机枪僵尸", "窝瓜僵尸", "高坚果僵尸",
            "红眼巨人僵尸"};
        return type >= 0 && type < int(std::size(names)) ? names[type] : "未知僵尸";
    }
    // 僵尸简称：直接去掉全称末尾的"僵尸"二字，不再维护单独的简称表。
    static std::string StripZombieSuffix(std::string name) {
        static const std::string suffix = "僵尸";
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            name.resize(name.size() - suffix.size());
        return name;
    }
    // 引擎 Plant::GetNameString(SeedType, SeedType) 是静态方法，经 tao.methods 空调用。
    // PlantType 与 SeedType 在 0..52 范围内编号一致，可直接传植物类型。
    std::string RuntimePlantName(int type, int imitater) {
        if (!host->methods.invoke || type < 0 || type >= int(tao::enums::PlantType::NUM_PLANT_TYPES.Value())) return {};
        TaoPluginValue arguments[2]{};
        arguments[0].struct_size = sizeof(arguments[0]);
        arguments[0].kind = TAO_VALUE_INTEGER;
        arguments[0].value.integer_value = type;
        arguments[1].struct_size = sizeof(arguments[1]);
        arguments[1].kind = TAO_VALUE_INTEGER;
        arguments[1].value.integer_value = imitater;
        TaoPluginValue result{};
        result.struct_size = sizeof(result);
        static constexpr std::string_view methodId = "Lawn.Plant.GetNameString(Lawn.SeedType, Lawn.SeedType)";
        if (host->methods.invoke(host->methods.host_context, tao::AbiString(methodId), {}, arguments, 2, &result) != TAO_PLUGIN_OK)
            return {};
        return result.kind == TAO_VALUE_STRING ? CopyView(result.value.string_value) : std::string{};
    }
    std::string RuntimeZombieName(int type) {
        if (!host->gameplay.definition || type < 0 || !objects || !objects->Available()) return {};
        TaoPluginObjectHandle handle{};
        handle.struct_size = sizeof(handle);
        if (host->gameplay.definition(host->gameplay.host_context, TAO_OBJECT_ZOMBIE_DEFINITION, type, &handle) != TAO_PLUGIN_OK)
            return {};
        auto definition = objects->Wrap<tao::ZombieDefinition>(handle);
        if (!definition) return {};
        const auto name = definition.Value()->GetName();
        return name ? name.Value() : std::string{};
    }
    // 种子定义刷新时间（冷却百分比的分母），经 tao.gameplay 原版定义查询，按类型缓存。
    int PlantRefreshTime(int seedType) {
        if (const auto found = plantRefreshCache.find(seedType); found != plantRefreshCache.end()) return found->second;
        int refresh = 0;
        if (host->gameplay.definition && objects && objects->Available() && seedType >= 0) {
            TaoPluginObjectHandle handle{};
            handle.struct_size = sizeof(handle);
            if (host->gameplay.definition(host->gameplay.host_context, TAO_OBJECT_PLANT_DEFINITION, seedType, &handle) == TAO_PLUGIN_OK)
                if (auto definition = objects->Wrap<tao::PlantDefinition>(handle))
                    if (const auto value = definition.Value()->GetRefreshTime())
                        refresh = value.Value();
        }
        plantRefreshCache.emplace(seedType, refresh);
        return refresh;
    }
    // 种子定义阳光消耗，经 tao.gameplay 原版定义查询，按类型缓存；失败缓存 -1。
    int SeedCost(int seedType) {
        if (const auto found = plantCostCache.find(seedType); found != plantCostCache.end()) return found->second;
        int cost = -1;
        if (host->gameplay.definition && objects && objects->Available() && seedType >= 0) {
            TaoPluginObjectHandle handle{};
            handle.struct_size = sizeof(handle);
            if (host->gameplay.definition(host->gameplay.host_context, TAO_OBJECT_PLANT_DEFINITION, seedType, &handle) == TAO_PLUGIN_OK)
                if (auto definition = objects->Wrap<tao::PlantDefinition>(handle))
                    if (const auto value = definition.Value()->GetSeedCost())
                        cost = value.Value();
        }
        plantCostCache.emplace(seedType, cost);
        return cost;
    }
    // 引擎 Plant 的静态布尔判定（IsAquatic/IsNocturnal 等），经 tao.methods 空调用。
    bool PlantStaticBool(std::string_view methodId, int seedType) {
        if (!host->methods.invoke || seedType < 0) return false;
        TaoPluginValue argument{};
        argument.struct_size = sizeof(argument);
        argument.kind = TAO_VALUE_INTEGER;
        argument.value.integer_value = seedType;
        TaoPluginValue result{};
        result.struct_size = sizeof(result);
        if (host->methods.invoke(host->methods.host_context, tao::AbiString(methodId), {}, &argument, 1, &result) != TAO_PLUGIN_OK)
            return false;
        if (result.kind == TAO_VALUE_BOOLEAN) return result.value.boolean_value != 0;
        if (result.kind == TAO_VALUE_INTEGER) return result.value.integer_value != 0;
        return false;
    }
    // 宿主 game_mode（LawnApp::mGameMode），仅诊断用。
    int CurrentGameMode() {
        if (!objects || !objects->Available()) return -1;
        auto app = objects->Root<tao::LawnApp>();
        if (!app) return -1;
        const auto mode = app.Value()->GetGameMode();
        return mode ? int(mode.Value().Value()) : -1;
    }
    // 经宿主 TaoEngineResolveObject 把 Board 语义句柄解成真实对象指针。
    // 失败（句柄失效/接口缺失）返回空。
    void* BoardPointer() {
        if (!currentBoard.object_id) return nullptr;
        return TaoEngineResolveObject(currentBoard);
    }
    // 读 Board::mZombieAllowed[type]（bool[100]，偏移见 kZombieAllowedOffset）。
    static bool ZombieAllowedAt(const void* board, int type) {
        return reinterpret_cast<const uint8_t*>(board)[kZombieAllowedOffset + size_t(type)] != 0;
    }
    // 临时诊断：进入每个棋盘时打印一次模式信息 + 读到的出怪集合，确认偏移与时机。
    void LogModeDiagnostics(const TaoPluginAccessibilityContextV1& context) {
        if (diagLogged || !log) return;
        diagLogged = true;
        std::string line = "模式诊断 scene=" + std::to_string(context.scene) + " game_mode=" + std::to_string(CurrentGameMode());
        if (objects && objects->Available() && currentBoard.object_id)
            if (auto boardProxy = objects->Wrap<tao::Board>(currentBoard))
                if (const auto value = boardProxy.Value()->GetLevel())
                    line += " level=" + std::to_string(value.Value());
        if (void* board = BoardPointer()) {
            line += " 出怪=";
            for (int type = 0; type < int(tao::enums::ZombieType::NUM_ZOMBIE_TYPES.Value()); ++type)
                if (ZombieAllowedAt(board, type)) line += std::to_string(type) + ",";
        } else {
            line += " 指针解析失败";
        }
        log(line);
    }
    // 引擎 Board::CanZombieSpawnOnLevel(ZombieType, int) 是静态方法，经 tao.methods 空调用。
    // 实现内部查 gZombieAllowedLevels 表，关卡号超出 1..50 会越界读，调用前先夹紧范围。
    bool ZombieSpawnsOnLevel(int type, int level) {
        if (!host->methods.invoke || type < 0 || level < 1 || level > 50) return false;
        TaoPluginValue arguments[2]{};
        arguments[0].struct_size = sizeof(arguments[0]);
        arguments[0].kind = TAO_VALUE_INTEGER;
        arguments[0].value.integer_value = type;
        arguments[1].struct_size = sizeof(arguments[1]);
        arguments[1].kind = TAO_VALUE_INTEGER;
        arguments[1].value.integer_value = level;
        TaoPluginValue result{};
        result.struct_size = sizeof(result);
        static constexpr std::string_view methodId = "Lawn.Board.CanZombieSpawnOnLevel(Lawn.ZombieType, int)";
        if (host->methods.invoke(host->methods.host_context, tao::AbiString(methodId), {}, arguments, 2, &result) != TAO_PLUGIN_OK)
            return false;
        if (result.kind == TAO_VALUE_BOOLEAN) return result.value.boolean_value != 0;
        if (result.kind == TAO_VALUE_INTEGER) return result.value.integer_value != 0;
        return false;
    }
    // 引擎 Plant::GetToolTip(SeedType) 是静态方法，经 tao.methods 空调用，按类型缓存。
    std::string PlantToolTip(int seedType) {
        if (const auto found = plantTooltipCache.find(seedType); found != plantTooltipCache.end()) return found->second;
        std::string tip;
        if (host->methods.invoke && seedType >= 0) {
            TaoPluginValue argument{};
            argument.struct_size = sizeof(argument);
            argument.kind = TAO_VALUE_INTEGER;
            argument.value.integer_value = seedType;
            TaoPluginValue result{};
            result.struct_size = sizeof(result);
            static constexpr std::string_view methodId = "Lawn.Plant.GetToolTip(Lawn.SeedType)";
            if (host->methods.invoke(host->methods.host_context, tao::AbiString(methodId), {}, &argument, 1, &result) == TAO_PLUGIN_OK &&
                result.kind == TAO_VALUE_STRING)
                tip = CopyView(result.value.string_value);
        }
        // tooltip 多行文本直接交给语音会断句生硬，折行换成空格。
        std::replace(tip.begin(), tip.end(), '\n', ' ');
        plantTooltipCache.emplace(seedType, tip);
        return tip;
    }
    std::string PlantDisplayName(int type, int imitater) {
        const int key = (type + 1) * 128 + imitater + 1;
        if (const auto found = plantNameCache.find(key); found != plantNameCache.end()) return found->second;
        std::string name = RuntimePlantName(type, imitater);
        const bool runtime = LooksLocalized(name);
        if (!runtime) {
            name = type == int(tao::enums::PlantType::PLANT_IMITATER.Value()) && imitater >= 0
                ? std::string("模仿者 ") + PlantNameFallback(imitater)
                : PlantNameFallback(type);
        }
        if (log && !loggedPlantNameSource) { // 临时诊断：确认名称解析路径
            loggedPlantNameSource = true;
            log(std::string("植物名称解析：") + (runtime ? "运行时 " : "内置表 ") + name);
        }
        plantNameCache.emplace(key, name);
        return name;
    }
    std::string ZombieDisplayName(int type, bool shortForm) {
        auto found = zombieNameCache.find(type);
        if (found == zombieNameCache.end()) {
            std::string name = RuntimeZombieName(type);
            const bool runtime = LooksLocalized(name);
            if (!runtime) name = ZombieNameFallback(type);
            if (log && !loggedZombieNameSource) { // 临时诊断：确认名称解析路径
                loggedZombieNameSource = true;
                log(std::string("僵尸名称解析：") + (runtime ? "运行时 " : "内置表 ") + name);
            }
            found = zombieNameCache.emplace(type, std::move(name)).first;
        }
        return shortForm ? StripZombieSuffix(found->second) : found->second;
    }

    bool ShovelTarget(TaoPluginShovelTargetV1& out) {
        size_t count = 0;
        host->board.shovel_target_count(host->board.host_context, currentBoard, column, row, &count);
        if (!count) return false;
        layer %= count;
        out = {};
        out.struct_size = sizeof(out);
        return host->board.shovel_target_at(host->board.host_context, currentBoard, column, row, layer, &out) == TAO_PLUGIN_OK;
    }
    void AnnounceLayer() {
        TaoPluginShovelTargetV1 target{};
        if (!ShovelTarget(target)) { Say("此格没有可铲目标"); return; }
        static const char* layers[] = {"普通植物", "南瓜", "底座", "飞行植物"};
        Say(layers[std::min<uint32_t>(target.layer, 3)]);
    }

public:
    // ===== 僵尸波次 / 快速僵尸提示音（迁移自 PvZA11y：Program.cs 主循环 + Board.cs 扫描）=====
    // 入场提示：新出现的僵尸按行播放正弦音，音高 1000 - row*500/行数 逐行递降，
    // 行间间隔 200ms（20 tick）；快速僵尸（撑杆/橄榄球/雪橇小队）总数增加时按
    // 最新个体的行立即播放三连锯齿音。音频为预渲染 wav（tools/GenCues.ps1 生成，
    // resources.xml 声明），声像已烘到右声道（原版左音量恒为 0）；实际音量走
    // tao.audio_group 的提示音增益（设置的 cues 滑块）。
    void ResetCues() {
        cueBoard = currentBoard;
        seenZombies.clear();
        pendingCues.clear();
        prevFastCount = 0;
        cueSeedIndex = -1;
        packetWasReady = false;
        waveCountdownLastSecond = -1;
        waveCountdownPrevCd = -1;
    }
    void UpdateCues(const TaoPluginAccessibilityContextV1& context, uint64_t tick, bool entryOn, bool fastOn, bool readyOn,
                    bool waveCountdownOn = true, int waveCountdownSeconds = 5) {
        lastCueTick = tick;
        PruneCueSounds(tick);
        UpdateWaveCountdown(context, tick, waveCountdownOn, waveCountdownSeconds);
        for (size_t i = pendingCues.size(); i > 0;) {
            --i;
            if (tick < pendingCues[i].dueTick) continue;
            PlayRowCue(pendingCues[i].row, gridFieldRows, pendingCues[i].fast, tick);
            pendingCues.erase(pendingCues.begin() + i);
        }
        if (tick % 10 != 5) return; // 僵尸扫描节流：100ms 一次
        if (!SameHandle(cueBoard, currentBoard)) ResetCues(); // 换关卡/换棋盘时重置检测
        std::string view;
        if (!ViewName('z', view)) return;
        // 提示音按场地高度（泳池/浓雾 6、其余 5）选 H5/H6 资源套，不用收窄后
        // 的可导航行数——僵尸行号是绝对编号，1-1 的行 2 对应 H5_R2（存在），
        // 用 gridRows=1 会拼出不存在的 H1_R2 且 zRow<height 过滤把僵尸全滤掉。
        const int height = std::clamp(gridFieldRows, 1, 6);
        bool entered[6] = {};
        int fastCount = 0, youngestAge = INT_MAX, fastRow = 0;
        ForEachViewObject(view, [&](TaoPluginObjectHandle handle) {
            auto zombie = objects->Wrap<tao::Zombie>(handle);
            if (!zombie) return;
            const auto z = zombie.Value();
            const auto dead = z->GetDead();
            if (!dead || dead.Value()) return;
            const auto age = z->GetAge();
            const auto zRow = z->GetRow();
            if (!age || !zRow) return;
            // 自己记录见过的个体（对象槽复用时 generation 变化，混合进键）；
            // 首次见到且足够"年轻"才算新入场，避免 mod 中途启用时把全场当新波次。
            const uint64_t key = handle.object_id ^ (handle.generation * UINT64_C(0x9E3779B97F4A7C15));
            const auto [it, fresh] = seenZombies.emplace(key, tick);
            it->second = tick;
            if (fresh && entryOn && age.Value() >= 0 && age.Value() < 80 &&
                zRow.Value() >= 0 && zRow.Value() < height)
                entered[zRow.Value()] = true;
            const auto type = z->GetZombieType();
            if (!type) return;
            const int value = int(type.Value().Value());
            if (value == int(tao::enums::ZombieType::ZOMBIE_POLEVAULTER.Value()) ||
                value == int(tao::enums::ZombieType::ZOMBIE_FOOTBALL.Value()) ||
                value == int(tao::enums::ZombieType::ZOMBIE_BOBSLED.Value())) {
                ++fastCount;
                if (age.Value() < youngestAge) { youngestAge = age.Value(); fastRow = zRow.Value(); }
            }
        });
        // 清理离场已久的记录，表不会无限增长
        if (tick % 500 == 5) {
            for (auto it = seenZombies.begin(); it != seenZombies.end();) {
                if (tick - it->second > 3000) it = seenZombies.erase(it);
                else ++it;
            }
        }
        if (entryOn) {
            for (int r = 0; r < height; ++r)
                if (entered[r] && pendingCues.size() < 16)
                    pendingCues.push_back({tick + uint64_t(r) * 20, r, false});
        }
        if (fastOn && fastCount > prevFastCount)
            PlayRowCue(std::clamp(fastRow, 0, height - 1), height, true, tick);
        prevFastCount = fastCount;
        // 当前选中卡片就绪提示（对齐 PvZA11y PlantReadyCueVolume 的上升沿判定）：
        // 冷却结束且阳光足够时播一次琶音。手动换槽（数字键/循环键）不同步触发：
        // 换槽瞬间按当前就绪状态直接同步，只保留选中期间自动就绪（冷却跑完/
        // 阳光攒够）的提示。
        // 老虎机关卡没有真正的种子银行：跳过 target_at(SELECT_SEED)。宿主对该
        // 关卡的老虎机专用种子包（SEED_SLOT_MACHINE_SUN/DIAMOND，类型 56/57）求
        // 名称/可用性会越界读植物定义表，进程崩溃（0xC0000005，实机排障结论：
        // 首次拉杆后第一个滚轮停下、专用包入银行时必现）。
        // 我是僵尸关卡：就绪提示同样绕开 target_at（名称错位），从 objects 枚举的
        // 僵尸包对象直接读冷却字段（GetRefreshCounter/GetRefreshTime），费用走
        // Board::GetCurrentPlantCost（生成代理；对僵尸包正确，实机已验证）。
        // 禅境花园没有种子银行：target_at(SELECT_SEED) 空枚举与上述崩溃同族风险，跳过。
        if (readyOn && IsIZombieLevel()) {
            bool ready = false;
            for (const auto& pk : BankPackets()) {
                if (pk.slot != seedIndex) continue;
                auto packet = objects->Wrap<tao::SeedPacket>(pk.handle);
                if (!packet) break;
                const auto counter = packet.Value()->GetRefreshCounter();
                const auto total = packet.Value()->GetRefreshTime();
                const bool refreshed = !counter || counter.Value() <= 0 ||
                    (total && total.Value() <= 0);
                ready = refreshed && context.sun >= ZombiePacketCost(pk.type);
                break;
            }
            if (seedIndex != cueSeedIndex) {
                cueSeedIndex = seedIndex;
                packetWasReady = ready;
            } else if (ready && !packetWasReady) {
                PlayCueSound("SOUND_PVZ_A11Y_READY", tick);
            }
            packetWasReady = ready;
        } else if (readyOn && !IsSlotMachineLevel() && !IsZenGarden() && !IsTreeOfWisdom() && !HasSpecialBank()) {
            bool ready = false;
            TaoPluginBoardTargetV2 target{};
            target.struct_size = sizeof(target);
            if (seedIndex >= 0 &&
                host->board.target_at(host->board.host_context, currentBoard, TAO_BOARD_SELECT_SEED, size_t(seedIndex), &target) == TAO_PLUGIN_OK &&
                target.type >= 0)
                ready = target.cooldown_remaining <= 0 && context.sun >= target.cost;
            if (seedIndex != cueSeedIndex) {
                cueSeedIndex = seedIndex;
                packetWasReady = ready;
            } else if (ready && !packetWasReady) {
                PlayCueSound("SOUND_PVZ_A11Y_READY", tick);
            }
            packetWasReady = ready;
        }
    }

private:
    void PlayRowCue(int cueRow, int height, bool fast, uint64_t tick) {
        char id[48];
        std::snprintf(id, sizeof id, "SOUND_PVZ_A11Y_%s_H%d_R%d", fast ? "FAST" : "ENTRY", height, cueRow);
        PlayCueSound(id, tick);
    }
    void PlayCountdownCue(int secondIndex, uint64_t tick) {
        if (secondIndex < 1 || secondIndex > 15) return;
        char id[48];
        std::snprintf(id, sizeof id, "SOUND_PVZ_A11Y_COUNTDOWN_%d", secondIndex);
        PlayCueSound(id, tick);
    }
    void UpdateWaveCountdown(const TaoPluginAccessibilityContextV1& context, uint64_t tick,
                             bool countdownOn, int countdownSeconds) {
        if (!countdownOn || countdownSeconds <= 0) {
            waveCountdownLastSecond = -1;
            waveCountdownPrevCd = -1;
            return;
        }
        if (!Gameplay(context) || IsSlotMachineLevel() || IsZenGarden() || IsTreeOfWisdom() ||
            IsVaseLevel() || IsIZombieLevel()) {
            waveCountdownLastSecond = -1;
            waveCountdownPrevCd = -1;
            return;
        }
        const auto boardHandle = context.board.object_id ? context.board : currentBoard;
        if (!objects || !objects->Available() || !boardHandle.object_id) {
            waveCountdownLastSecond = -1;
            waveCountdownPrevCd = -1;
            return;
        }
        auto boardResult = objects->Wrap<tao::Board>(boardHandle);
        if (!boardResult) {
            waveCountdownLastSecond = -1;
            waveCountdownPrevCd = -1;
            return;
        }
        const tao::Board boardProxy = boardResult.Value();
        const auto levelComplete = boardProxy->GetLevelComplete();
        if (levelComplete && levelComplete.Value()) {
            waveCountdownLastSecond = -1;
            waveCountdownPrevCd = -1;
            return;
        }
        const auto numWaves = boardProxy->GetNumWaves();
        const auto curWave = boardProxy->GetCurrentWave();
        if (numWaves && curWave && numWaves.Value() > 0 && curWave.Value() >= numWaves.Value()) {
            waveCountdownLastSecond = -1;
            waveCountdownPrevCd = -1;
            return;
        }
        const auto cdResult = boardProxy->GetZombieCountdown();
        if (!cdResult) {
            waveCountdownLastSecond = -1;
            waveCountdownPrevCd = -1;
            return;
        }
        const int cd = cdResult.Value();
        const int warningLimit = std::clamp(countdownSeconds, 1, 15);
        const int warningTicks = warningLimit * 100;
        if (cd <= 0 || cd > warningTicks) {
            waveCountdownLastSecond = warningLimit + 1;
            waveCountdownPrevCd = cd;
            return;
        }
        if (waveCountdownPrevCd > 0 && cd > waveCountdownPrevCd + 50) {
            waveCountdownLastSecond = std::max((cd + 99) / 100 + 1, warningLimit + 1);
        }
        if (waveCountdownLastSecond < 0) {
            waveCountdownLastSecond = warningLimit + 1;
        }
        const int currentSecond = std::clamp((cd + 99) / 100, 1, warningLimit);
        if (waveCountdownLastSecond > currentSecond) {
            PlayCountdownCue(currentSecond, tick);
            waveCountdownLastSecond = currentSecond;
        }
        waveCountdownPrevCd = cd;
    }
    void PlayCueSound(const char* id, uint64_t tick) {
        if (!host->sound.play) return;
        TaoPluginAudioPlayDescriptorV1 descriptor{};
        descriptor.struct_size = sizeof(descriptor);
        descriptor.resource_id = tao::AbiString(id);
        descriptor.volume = 1.0; // 增益由 tao.audio_group 统一控制
        TaoPluginAudioHandle sound{};
        if (host->sound.play(host->sound.host_context, &descriptor, &sound) != TAO_PLUGIN_OK) {
            if (log && !cuePlayFailed) { cuePlayFailed = true; log(std::string("提示音播放失败：") + id); }
            return;
        }
        activeCueSounds.push_back({sound, tick});
    }
    // 一次性实例播完必须 destroy 归还宿主混音通道：只查 is_valid 不够
    // （播完的实例句柄仍可能有效），以 describe 的 PLAYING 标志为准，
    // 超过 3 秒（300 tick，最长提示音 250ms）无条件兜底回收。
    void PruneCueSounds(uint64_t tick) {
        if (!host->sound.destroy) { activeCueSounds.clear(); return; }
        for (size_t i = activeCueSounds.size(); i > 0;) {
            --i;
            bool finished = tick - activeCueSounds[i].playedTick > 300;
            if (!finished && host->sound.describe) {
                TaoPluginAudioInfoV1 info{};
                info.struct_size = sizeof(info);
                if (host->sound.describe(host->sound.host_context, activeCueSounds[i].sound, &info) == TAO_PLUGIN_OK)
                    finished = !(info.flags & TAO_AUDIO_PLAYING);
            }
            if (!finished) continue;
            host->sound.destroy(host->sound.host_context, activeCueSounds[i].sound);
            activeCueSounds.erase(activeCueSounds.begin() + i);
        }
    }

    const HostServices* host;
    std::function<void(std::string, bool)> say;
    std::function<void(std::string)> log;
    const tao::Objects* objects = nullptr;
    bool resolvedViews = false;
    bool zombieShortNames = false;
    std::string zombiesView;
    std::string gridItemsView;
    std::string coinsView;
    std::string mowersView;
    std::function<int(int)> loadGridRows;        // 冒险 1-1..1-3 可玩行数档案记忆（SetGridMemory）
    std::function<void(int, int)> saveGridRows;
    std::string seedPacketsView;
    std::unordered_map<int, std::string> plantNameCache;
    std::unordered_map<int, std::string> zombieNameCache;
    std::unordered_map<int, int> plantRefreshCache;
    std::unordered_map<int, int> plantCostCache;
    std::unordered_map<int, std::string> plantTooltipCache;
    bool loggedPlantNameSource = false, loggedZombieNameSource = false; // 临时诊断
    TaoPluginObjectHandle currentBoard{};
    int column = 0, row = 0, seedIndex = 0;
    // 提示音检测状态
    TaoPluginObjectHandle cueBoard{};
    std::unordered_map<uint64_t, uint64_t> seenZombies; // 混合键 -> 最后见到 tick
    struct PendingCue { uint64_t dueTick; int row; bool fast; };
    std::vector<PendingCue> pendingCues;
    struct ActiveCue { TaoPluginAudioHandle sound; uint64_t playedTick; };
    std::vector<ActiveCue> activeCueSounds;
    int prevFastCount = 0;
    int cueSeedIndex = -1;
    bool packetWasReady = false;
    bool cuePlayFailed = false;
    int waveCountdownLastSecond = -1;
    int waveCountdownPrevCd = -1;
    uint64_t cycleZombieKey = 0; // 循环查看僵尸的当前个体（棋盘切换时归零）
    uint64_t lastCueTick = 0;
    uint64_t boardTick = 0; // OnUpdate 喂入（NoteTick），ReadCell 路径提示音用
    // 宝石迷阵/旋风"可达匹配"提示音状态：已评估格/结果/网格失效标记/上次哔声格。
    int matchCueCheckColumn = -1, matchCueCheckRow = -1;
    bool matchCueCheckResult = false;
    bool matchCueGridDirty = true;
    int matchCueBeepColumn = -1, matchCueBeepRow = -1;
    // 特殊操作包"单击朗读/双击执行"状态（SelectSpecialBankPacket）。
    int specialBankLastSlot = -1;
    std::chrono::steady_clock::time_point specialBankLastPressedAt{};
    size_t layer = 0;
    bool boardFocus = false;
    std::string autoGate, autoSeen, autoFailed; // 自动收集临时诊断状态
    std::vector<TaoPluginObjectHandle> autoAnnounced; // 已确认进入收集流程的奖励硬币
    bool diagLogged = false; // 临时诊断：每个棋盘打印一次模式信息
    bool bossBallResolveLogged = false; // 临时诊断：每个棋盘打印一次僵王球位置解析结果
    // 禅境花园状态
    bool zenGarden = false;          // 模式探针结果（随棋盘缓存）
    bool zenProbeValid = false;       // 探针是否已按当前棋盘求值
    bool treeOfWisdom = false;        // 智慧树模式探针结果（与 zenGarden 同次求值，
                                      // 花园家族内 43↔50 就地切换时每次重求，见 ProbeZenMode）
    bool gridProbeValid = false;      // 草坪网格探针是否已按当前棋盘求值（随棋盘缓存）
    int gridColumns = 9;              // 草坪实际列数（ProbeGrid，默认宿主上报值）
    int gridRows = 6;                 // 草坪实际行数（泳池/浓雾 6，其余 5，冒险前几关更少）
    int gridRowMin = 0;               // 有效行起点（冒险 1-1=2、1-2/1-3=1，行号保持绝对编号）
    int gridFieldRows = 6;            // 场地高度（泳池/浓雾 6、其余 5），僵尸提示音选 H5/H6 资源套用
    bool zenCompose = false;         // ComposeCellText 期间缓存，供 PlantSegment 用
    TaoPluginObjectHandle gardenCarried{}; // 手套搬移中的植物句柄
};
