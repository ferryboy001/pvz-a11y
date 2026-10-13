# PvZ 无障碍辅助（pvz.a11y）

基于 Tao Native SDK 2.0 语义 ABI 的 PvZ 无障碍 Mod 框架，用于替代
[PvZA11y](https://github.com/CG8516/PvZA11y) 的 C# 外部进程 + 内存读写 + 字节补丁方案。
所有与游戏的交互（棋盘、控件、文本、输入）均经由宿主提供的版本化接口完成，不接触游戏内存。

## 模块结构

| 文件 | 职责 | 对应 PvZA11y 组件 |
| --- | --- | --- |
| `src/HostServices.hpp` | 七个宿主语义服务的连接与持有 | `MemoryIO.cs`（整体被语义 ABI 取代） |
| `src/Speech.hpp` | 语音输出（prism / SAPI，独立线程、自动切换后端） | Tolk / NVDA / JAWS / SAPI 后端 |
| `src/Narrator.hpp` | 无障碍事件订阅与轮询分发 | `Program.cs` 的文本叙述主循环 |
| `src/BoardControl.hpp` | 格子光标、种子包、收集、关卡末尾奖励自动领取、铲子、冻结 | `Program.cs` 的棋盘操作部分 |
| `src/UiNavigation.hpp` | 控件树遍历、焦点导航、激活/调整/取消 | `Widgets/` 目录 |
| `src/EditFeedback.hpp` | 编辑框当前值、UTF-8 插入点与输入/删除差量朗读 | — |
| `src/ChallengeNames.hpp` / `src/ChallengeScreenState.cpp` | 挑战选择界面关卡按钮命名（跟踪进入与页内切换，bounds 反查 gChallengeDefs，包含 Limbo 状态页） | — |
| `src/Store.hpp` | 疯狂戴夫的商店：开/退店入口、商品浏览朗读、原生购买链（门禁/确认框）、翻页与返回按钮命名 | — |
| `src/InputMap.hpp` | 可重绑定按键映射表与查找 | `Input.cs` / `InputRebind.cs` |
| `src/Settings.hpp` | 玩家档案内的设置持久化 | `Config.cs` |
| `src/Plugin.cpp` | 生命周期、事件接线、动作分发 | `Program.cs` 主类 |

## 挑战关卡列表

挑战页面的 Limbo（状态页）沿用宿主本地化关卡名称，缺少译文时使用中文名表。
页按钮会直接改变 `ChallengeScreen::mPageIndex`，不会重新调用
`LawnApp::ShowChallengeScreen`；通过 `ChallengeScreen::UpdateButtons` 同步页码，
支持从生存、小游戏、解谜页切入状态页及返回。状态页包含 20 个关卡按钮。
`tests/ChallengeNames.cpp` 回归覆盖原地翻页、全部状态页名称、返回各页与关闭页面。

## 已迁移的音频提示

- 每波僵尸入场：按行播放递降音高的正弦提示音（音高 = 1000 - 行号*500/行数，
  与 PvZA11y 的 `ZombieEntryVolume` 公式一致），行间间隔 200ms，仅右声道。
  检测由 mod 侧记录已见个体（对象句柄 + generation），不写游戏内存
  （PvZA11y 原为改写僵尸 age 字段做标记）。
- 快速僵尸（撑杆/橄榄球/雪橇小队）：场上总数增加时按最新个体所在行播放
  三连锯齿提示音（50ms x 3，同音高公式），仅右声道。
- 巨人/红眼巨人举锤：每次进入 `PHASE_GARGANTUAR_SMASHING` 时按所在行播放
  双声锯齿音（50ms x 2，间隔 50ms，同音高公式）。声像按各巨人的 `pos_x`
  在 800px 逻辑棋盘上的位置设置（左/中/右），不受窗口缩放影响。持续举锤不重播，
  投掷小鬼不触发；同行多个巨人也分别定位。阶段读取失败保留检测状态。
- 网格跨行僵尸提示：上下移动并实际改变行号时，将本行数量或 Shift 同款的数量、
  列号、名称及状态追加到格子文本，合并成一条语音。`row_zombie_speech` 为 0 关闭、
  1 数量（默认）、2 详细；`row_zombie_cue` 独立开关三角波（默认开），
  `row_zombie_volume` 为独立音量（0–100，默认 100）。100ms 三角波沿用
  `1000-row*500/height` 音高，由最左存活僵尸 `pos_x` 决定声像；资源为
  `SOUND_PVZ_A11Y_ROW_H{5,6}_R{row}`。使用绝对行号及场地高度，空行、边界、
  横向移动、重读、模态、禅境花园/智慧树不播放。位置不可读时保留数量，音效静默。
  F11 提供朗读模式、音效开关和音量；音量调整后试听，回车重播居中 H5_R2 三角波。
- 当前选中卡片就绪：冷却结束且阳光充足时播放上升琶音（698.46/880/1046.5 Hz
  正弦叠加，双声道，对齐 PvZA11y 的 `PlantReadyCueVolume`）；手动换槽
  （数字键/循环键）不触发，只在卡片选中期间自动就绪（冷却跑完/阳光攒够）
  时播一次。判定只读 `target_at` 的 `cooldown_remaining` 与 `cost`
  （阳光取自无障碍上下文）。
- 下一波僵尸刷新倒计时：以短促方波（60ms，C 大调自然音阶）在波次刷新前按秒
  倒计时播报。音调从高到低逐秒递降一个音，最终在刷新前 1 秒到达 C5（默认 5 秒倒计时
  即 G F E D C）。设定的提醒时间越长，起始音高相应提高（如 7 秒从 B5 开始）。
  实时追踪 `zombie_countdown`，当快速击杀僵尸导致刷新倒计时突降、或波次重置时均能即刻反应。
- 实现：`BoardControl::UpdateCues`（100ms 节流扫描 zombies 视图，波次倒计时每 tick
  精准更新）+ `tao.audio` 播放预渲染 wav。wav 由 `tools/GenCues.ps1` 生成，
  `resources.xml` 声明为 `SOUND_PVZ_A11Y_ENTRY/FAST/HAMMER_H{行数}_R{行号}`、
  就绪琶音为 `SOUND_PVZ_A11Y_READY`、倒计时音为 `SOUND_PVZ_A11Y_COUNTDOWN_{1..15}`；
  音量走 `tao.audio_group` 的提示音增益（`cues` 设置）。开关与参数：档案设置
  `zombie_entry_cue` / `fast_zombie_cue` / `gargantuar_hammer_cue` / `plant_ready_cue` / `wave_countdown_cue`（默认开）
  以及 `wave_countdown_seconds`（默认 5）。
  F11 设置面板新增 `gargantuar_hammer_volume`、`wave_countdown_volume`、
  `zombie_entry_volume` 三项独立音量（整数 0–100%，默认 100），按玩家档案持久化，
  再叠加 `cues` 总增益。上下/Tab 切换，左右每次调整 5%，调整立即试听新音量，回车重播；
  鼠标可点击减小/增大或音量值。试听复用实际播放的资源和独立增益，举锤示范居中、
  倒计时示范为 G5、入场示范为 H5_R2。连续试听替换前一实例，关闭面板停止试听；
  普通和嵌套模态帧均回收音频，避免在设置里积累宿主混音通道。
  `tests/GargantuarCues.cpp` 覆盖两种巨人、举锤去重/再次攻击、左右声像、五/六行音高资源、
  对象槽复用、阶段读取失败、开关与换棋盘；音高使用场地高度和绝对行号。
  音量与试听回归见 `tests/Logging.cpp`、`tests/GargantuarCues.cpp`、`tests/WaveCountdown.cpp`，
  覆盖键盘/鼠标、边界、保存失败、即时试听、独立增益和音频回收。

## 玉米炮与 mod 植物适配

- 空手在炮所在格回车激活，方向键移动后回车发射；退格取消瞄准。
  C 自动挑选一门就绪炮向当前格发射，优先行号、再列号，一次一门。
  未就绪、死亡、被压扁、睡眠、不在棋盘或正被蹦极抓取的炮不能参与快捷发射。
- 识别依据 `Plant::state` 的 `STATE_COBCANNON_ARMING/LOADING/READY/FIRING`
  （35..38，就绪=37），不以植物名称、动态内容 ID 或原版 SeedType 为必需条件。
  原版 PlantType 仅作初始化状态的兜底。格子命中由 `GetTopPlantAt` 处理植物占格。
- 激活通过植物自身 `MouseDown`（真实 `GetPlantRect` 中心），以
  `CURSOR_TYPE_COBCANNON_TARGET`（8）核对；发射走 `TAO_BOARD_FIRE_CANNON`，
  保留宿主棋盘门禁。没有候选时不改持卡状态，快捷发射不移动目标格光标。
- 避开宿主 `TAO_BOARD_SELECT_CANNON` 的 `mSeedType==SEED_COBCANNON` 限制。
  20261005 exe 的 `Board::FireCobcannonAt` 真身（RVA 0x8369c0，公开入口
  0x859460）只检查光标类型、所选 PlantID 有效/未死、state==37，然后调用
  `Plant::CobCannonFire` 并清光标；没有种子编号限制，也不等待鼠标防连点计时，
  因此 C 可以在同一次更新里选炮并发射。
- 新 mod 炮遵循以上状态/光标协议并实现其点击及发射行为即可自动适配。
  完全自定义状态或自定义瞄准光标的植物，需要另行提供能力协议；当前 SDK 没有
  通用“手动瞄准武器”能力字段，不能仅从名称或存在 `CobCannonFire` 方法推断
  （所有 Plant 代理都暴露该方法）。
- 回归测试：配置时加 `-DPVZ_A11Y_BUILD_TESTS=ON`，构建后在构建目录运行
  `ctest --output-on-failure`。`tests/CannonControl.cpp` 通过模拟宿主 ABI 验证
  两段回车、两格占位、取消、快捷选炮/目标保持、无炮时持卡不变、不可用炮排除、
  棋盘门禁、动态类型和点击假成功；不修改玩家档案。

## 编辑框朗读

- `UiNavigation` 在聚焦时读标题、当前值和插入点；Mod UI 编辑框用同父控件下相邻的
  静态文字补标题，兼容工具箱横排数值字段和上方标题的阵型代码字段。
- `EditFeedback` 按 UTF-8 字节边界比较前后文字，以字符计数报告插入点；移动读越过的
  文字，输入/删除读差量，F1 重读当前字段。原生编辑与剪贴板按键保留。
- 每次更新只在 `TAO_INPUT_TEXT_EDIT` 状态采样一个焦点节点，避免额外枚举整棵树，
  也避免访问老虎机/我是僵尸的危险 Board 控件；嵌套模态帧同样可采样。
- 测试开关沿用 `PVZ_A11Y_BUILD_TESTS`：`EditReading.cpp` 覆盖字段标题、借用字符串、
  聚焦去重、输入/删除、中文插入点、销毁与句柄复用；`CannonControl.cpp` 另覆盖花瓶朗读。

## 待迁移功能（按模块落点）

- 音频提示（僵尸声纳、位置提示音、植物就绪音）：`BoardControl` + `tao.audio` / `tao.audio_group`
- 僵尸循环查看、绊线播报：`BoardControl`（敌人状态用对象 SDK 读取）
- 自动收集阳光金币（关卡末尾奖励已由 `AutoCollectRewards` 自动领取）：`BoardControl` + `Settings` 开关
- 手柄支持：`InputMap`（`TAO_INPUT_EVENT_GAMEPAD_*` 已可用）+ 震动
- 按键重绑定界面：`InputMap` 表改为从档案加载 + `UiNavigation` 面板
- 触摸探索：SDK 自带 `tao/TouchExplorer.hpp`，接入 `Plugin.cpp` 输入回调
- 画面减弱（闪烁/震动）：`tao.accessibility.preferences`
- 迷阵/水族馆等特殊模式动作（砸罐已实现，见上文）：`BoardControl::Invoke` 已支持全部 `TAO_BOARD_*` 动作
- 多语言文本：`LawnStringsText.cs` / `Language/` 对应宿主事件与 `cell_text` 的中文叙述

## 编译与安装

```powershell
cmake -S pvz-a11y -B build/pvz-a11y -G Ninja -DTAO_NATIVE_SDK=<SDK>/engine-sdk
cmake --build build/pvz-a11y --config Release
```

将 `build/pvz-a11y/package` 整体复制为游戏资源目录下的 `mods/pvz-a11y`，
在游戏 Mod 管理界面启用并确认原生插件信任。不要只复制 DLL。
Windows 需要安装能朗读中文的 SAPI 语音；无语音时叙述只写入日志。

## 操作方式（默认与 PvZA11y 一致）

| 按键 | 手柄 | 动作 |
| --- | --- | --- |
| 方向键 | 十字键 | 战斗中移动格子；界面切换控件、调整数值；宝石迷阵按住 Ctrl 拖动植物 |
| Enter | A | 确认 / 种植 / 铲除（持铲时）/ 砸花瓶（光标空格子有花瓶时）/ 宝石迷阵旋风旋转当前格 / 僵尸水族馆投喂鱼食 |
| Backspace | B | 放回手上的卡片或工具 / 拒绝菜单 |
| Esc | Start | 暂停菜单（原生处理） |
| Tab | Back | 冻结/继续游戏；界面中切换控件 |
| - / = | LB / RB | 循环选择种子包；界面中切换控件 |
| F1 | X | 重读（僵尸声纳待迁移） |
| F2 | Y | 当前格植物与对象信息 |
| F3 | L3 | 阳光与金币数量 |
| F4 | R3 | 关卡进度 |
| 1–9、0 | — | 选第 1–10 个数字卡槽：种子银行在前，场上掉落的植物卡在后（老虎机无种子银行）；宝石迷阵/旋风/僵尸水族馆改选特殊操作（重新洗牌、填平弹坑、升级植物、买潜水僵尸、领奖杯）；禅境花园改选工具（1 浇水壶 2 肥料 3 杀虫剂 4 留声机 5 巧克力 6 手套 7 卖植物 8 手推车，0 前往下一个花园） |
| C | — | 自动选择一门就绪玉米炮，向当前格发射；没有可发射的炮时提示 |
| , / . | LT / RT | 循环查看僵尸（播报列行与状态，格子光标跟随到僵尸所在格） |
| F6 | — | 切换僵尸简称/全称 |
| Shift | — | 单击：按列从左到右播报本行僵尸（数量；列号，状态前缀+名称……，含僵王博士的冰球/火球）；双击：播报本行小推车（无则朗读"无"）；选卡界面：朗读本关出怪列表；禅境花园：播报花园总览（各需求数量、臭臭状态） |
| ` | — | 选择铲子，再按切换同格铲除层（禅境花园禁铲：铲掉盆栽等于丢弃玩家收集的植物） |

框架自身按键：F7 打开疯狂戴夫的商店，F8 开关无障碍，F9 朗读帮助，N 冻结时推进一帧。
键位表定义在 `src/InputMap.hpp`，后续由重绑定界面接管。

主菜单采用固定浏览顺序：冒险模式 → 迷你游戏 → 解谜模式 → 生存模式 → 禅境花园 →
商店 → 图鉴 → 成就 → 僵尸大头贴 → 更改用户 → 联机模式 → 选项 → 帮助 → 退出。
若当前版本显示快速游戏，排在冒险模式之后。
下/右/Tab 前进，上/左/Shift+Tab 后退，首尾循环；隐藏、禁用或不可聚焦的控件自动跳过。
顺序不受按钮大小、动画位置或控件创建顺序影响。弹窗和其它界面仍按原有策略导航。

主菜单就绪后自动朗读“欢迎回来，我的朋友”、当前存档名称和冒险关卡（如 1-1）；
切换或重命名用户、冒险进度改变后自动更新，弹窗和用户面板期间不播报底层菜单。
F2 重读完整主菜单信息，F4 重读冒险关卡；聚焦冒险模式时补读关卡，聚焦更改用户时
补读当前存档与“如果这不是你的存档，请点我”。按钮命名优先读取 GameSelector 的
实际按钮字段和边界（src/MainMenuState.cpp），坐标表仅作兜底；本版 (32,180) 是
僵尸大头贴，(49,136) 是更改用户，(265,364) 是联机模式。
主菜单进度读自 GameSelector::mLevel，名称读自当前 PlayerInfo::mName；只复制值，
不跨帧保存原生对象。无有效读数时不会猜测为 1-1。

## 语音后端

Mod 设置面板（`LoggingPanel.hpp`）整合了日志与辅助功能开关：
包含“启用 Trace 日志”、“启用 Mod 日志”（宿主 Mod 配置 schema，默认开）以及“波次刷新倒计时提示”（玩家档案配置，默认开）。
F11 打开本 Mod 设置面板；也可在“选项 → 额外设置 → Mod”列表中聚焦“PvZ 无障碍辅助”并按回车打开。
方向键、Tab / Shift+Tab 切换项目，回车切换开关，Esc / 退格 / F11 关闭并归还焦点。修改立即保存并生效，无需重启；外部配置更改每秒刷新。
Trace 开关控制 `mods/trace.log` 文件追加；Mod 开关控制 pvz-a11y 的所有 `LogInfo` 输出（含朗读文本和
F12 控件树）。关闭日志不影响实际语音与辅助功能，不删除已有内容；共享 `mods/mod.log`
仍由宿主及其它 Mod 使用。加载时只声明 schema 和读取设置，不覆盖已保存的值。

宿主本版 Mod 管理页只提供启用和排序，不自动生成 schema 配置页；`LoggingPanel.hpp` 使用
`tao.ui` 模态复选框呈现日志配置，并在原生多层弹窗的嵌套帧中处理输入。
`UiNavigation::RediscoverDialog` 在停订期间恢复当前顶层弹窗锚点与可用焦点，嵌套帧支持
方向键、Tab / Shift+Tab，避免进入 Mod 管理器后仍困在被遮挡的“额外设置”子树。

优先使用 prism（自动协商 NVDA / JAWS / SAPI 等读屏，
对应 PvZA11y 的 Tolk 通道）；`prism.dll` / `tolk.dll` 随 `native/windows_x64/` 一起部署，
运行时按插件自身路径加载。prism 缺失时回退到 Windows SAPI，两者皆无则只写日志。
Prism 项目仓库：<https://github.com/ethindp/prism>。
预置路径由 CMake 缓存变量 `PRISM_ROOT` 指定（默认源码树内 `prism-bin/windows-x64`）。
仓库不包含 `prism-bin/`；如需 prism 后端，请自行将预编译包放到上述路径，
或在配置时指定 `PRISM_ROOT`。没有该依赖时会构建仅使用 SAPI 的版本。

运行中关闭或启动读屏时会自动重新选择可用的最高优先级后端，例如 ZDSR → NVDA，
无读屏时使用 prism 的系统语音后端（不可用时回退 Windows SAPI），之后启动读屏也会切回。
语音线程每秒复查后端的运行时可用性，读屏 API 尚未就绪时继续重试。
随包 prism 0.18.2 开启 availability callback 后，独立程序实测在 shutdown 时
访问冲突（`prism.dll+0x7FCA5`），因此使用 mod 自己的线程检测，不开启其后台枚举。
切换后沿用当前音量、语速，
清除旧后端的待播内容与重复文本记录；播报失败会重新选择后端并重试当前文本一次。
所有后端操作都在独立语音线程串行执行，游戏输入和更新回调只负责入队，
避免后端检测、初始化或读屏响应缓慢触发宿主回调超时。关闭 mod 时停止并释放后端、
关闭 prism 上下文，再退出语音线程；已钉住的读屏 API DLL 仍保留到进程退出。

### 分发排障：别人机器上 prism 不生效只回退 SAPI

`native/windows_x64/` 必须整体分发（`plugin.dll`、`plugin.dll.tao-engine`、`prism.dll`、`tolk.dll`）；
mod 启动时会把 prism 加载失败的具体环节写入 `mods/mod.log`（"语音后端：sapi" 后紧跟一行
`prism: ...` 诊断），常见原因对照：

- `未找到 ...prism.dll`：压缩包漏文件、只更新了 plugin.dll，或被杀毒软件隔离。
- `LoadLibraryExW 失败 ... err=126`：依赖缺失（正常部署不应出现，prism.dll 仅依赖系统 DLL）。
- `导出符号缺失`：打包的 prism.dll 与构建时不一致。
- `prism_init 返回空上下文` / `后端 initialize 失败` / `create_best 无可用后端`：
  prism 本体加载成功，但读屏协商或后端初始化失败，把错误码发回排查。

## 播报策略

- 用户主动操作（方向键、确认等）：打断当前语音，立即播报。
- 宿主事件（场景、焦点、弹窗标题、普通文本）：排队播报，不打断。
- 弹窗打开：正文打断旧播报，首个控件与标题随后排队朗读。
- 输入事件回调只入队不执行（回调超时会被宿主移除），动作统一在 `OnUpdate` 执行。

## 持铲/持卡换卡的宿主陷阱（实机排障结论）

- 持卡时直接调语义 `SELECT_SHOVEL`：宿主强制收回手上卡包，但隐式收回走
  WasPlanted 路径，把该卡重置进冷却，此后这张卡 `SELECT_SEED` 永远报
  COOLDOWN、原生点击也点不动（表现为"锁死"）。因此 `SelectShovel` 先语义
  CANCEL 正常放回卡包，再拿铲子，且拿铲优先走原生点击铲子槽。
- 语义 CANCEL 对铲子只改 a11y 上报状态，真实光标不放回；放回铲子必须
  原生点击铲子槽（`NativeReturnShovel`）。
- 光标非空闲时换卡走原生点击卡包（`NativeSelectSeed`，卡槽矩形取
  SeedPacket 对象实测值，棋盘局部坐标），语义 `SELECT_SEED` 仅作兜底。

## 僵尸循环查看与 Whack-A-Zombie

- `,` / `.` 循环查看僵尸：按对象身份（句柄+generation）排序循环，播报
  "列行，状态前缀+名称"（列由 (posX+75) 换算 80px 列，出界报"场外"），
  播对应行音高提示音（复用入场音，声像固定右侧；PvZA11y 另按 posX 声像），
  格子光标跟随到僵尸所在格（对齐 PvZA11y 的 MoveOnZombieCycle）。
- Whack-A-Zombie 模式：光标为空时回车 = `TAO_BOARD_WHACK` 锤击当前格
  （最上层活僵尸）；持卡时回车仍放卡。配合循环查看的光标跟随可纯键盘游玩。

## 僵王博士冰球/火球与巨人状态

- 僵尸状态前缀（格子朗读、Shift 行播报、循环查看共用）新增：巨人/红眼巨人
  血量 ≤50% 报"半血"、投掷动画中报"投掷小鬼"；小鬼被抛出飞行中报"被投掷"
  （phase 判定，见 `BoardControl.hpp` 的 `ZombieStatusPrefix`）。
- 僵王博士的火球/冰球不是投射物，而是博士僵尸（`ZOMBIE_BOSS`）身上的
  reanimation（`mBossFireBallReanimID`，行号取 `mFireballRow`、冰火取
  `mIsFireball`）。网格浏览在球所在格朗读"僵王火球/僵王冰球"，Shift 行播报
  与僵尸同表列出。列坐标宿主语义接口未暴露，经宿主导出
  `LawnApp::ReanimationTryToGet`（/alternatename 链接，同 DoNewOptions 模式；
  `TaoEngineResolveObject` 对手工构造的 reanimation 句柄代际校验不过）解出指针、
  按 SDK `Reanimator.h` 布局读
  `mOverlayMatrix` 平移分量（X=+0x34、Y=+0x40，20261005 版 exe；换版本需按
  布局重确认）；解析失败回退"列不明"（网格浏览退化为整行朗读）。
  每棋盘首球解析结果写入 mod.log（"僵王球位置诊断"）。

## 砸花瓶（Vasebreaker / Scary Potter）

- 格子朗读按 `GridItem.transparent_counter > 0` 判断游戏当前透视状态，内容由
  `scary_pot_type` + `seed_type` / `zombie_type` 命名，例如“豌豆射手花瓶”“阳光花瓶”。
  路灯花和工具箱透视都受支持。未透视时只依据 `state` 的 LEAF(4)/ZOMBIE(5)
  瓶身标记朗读“植物花瓶”/“僵尸花瓶”，问号瓶不透露隐藏内容；字段不可读时保留瓶身名。
- 光标为空（`CURSOR_TYPE_NORMAL`）且当前格有花瓶时，**回车 = 砸罐**
  （`TAO_BOARD_BREAK_VASE`，目标是该格的 `GridItem`/`GRIDITEM_SCARY_POT`）。
  花瓶句柄经 `grid_items` 视图按 `GetGridX/GetGridY/GetType` 反查（与格子地形
  播报同一条路径）；宿主仅在 `IsScaryPotterLevel()` 关卡放行该动作。
- **卡槽统一编号**：砸花瓶关卡种子银行里可能有植物、场上也可能有砸罐掉落的植物卡，
  两者并存。`SelectCardSlot()` 把数字槽编号为**种子银行在前（`TAO_BOARD_SELECT_SEED`）、
  场上掉落卡在后（`TAO_BOARD_SELECT_CARD`）**，数字键与 `-`/`=` 循环键都走它。掉落卡用
  `DroppedCards()` 枚举（`COIN_USABLE_SEED_PACKET`，与老虎机奖励掉卡共用）；与老虎机
  一致：不走 `target_at(SELECT_CARD)`（无卡可枚举时宿主崩溃），改由 `coins` 视图自枚举后
  把句柄喂给 `BoardInvoke`。老虎机无种子银行（仅掉落卡）。
- 拾到卡后光标持卡（银行卡为 `CURSOR_TYPE_PLANT_FROM_BANK`、掉落卡为
  `CURSOR_TYPE_PLANT_FROM_USABLE_COIN`），回车即种到当前格（宿主 `TAO_BOARD_PLANT`
  支持两种光标）；此时回车不再砸罐（光标非空）。

## 宝石迷阵 / 宝石迷阵旋风 / 僵尸水族馆（无常规种子银行的特殊关卡）

这三关（`GAMEMODE_CHALLENGE_BEGHOULED`=20、`BEGHOULED_TWIST`=24、
`ZOMBIQUARIUM`=23）种子银行里不是植物卡，而是**特殊操作包**：宝石迷阵的
"重新洗牌"（`SEED_BEGHOULED_BUTTON_SHUFFLE`=54，100 阳光）与"填平弹坑"
（`SEED_BEGHOULED_BUTTON_CRATER`=55，200 阳光）、植物升级包（普通植物类型：
双发 7=1000、大喷菇 10=500、高坚果 23=250）；水族馆的"潜水僵尸"
（`SEED_ZOMBIQUARIUM_SNORKLE`=58，100 阳光）与"奖杯"
（`SEED_ZOMBIQUARIUM_TROPHY`=59，1000 阳光）。宿主语义 ABI 已内置全部
所需动作，mod 只做键位映射与播报：

- **数字键 / `-`/`=` 循环键选特殊操作：单击朗读、双击执行**
  （`SelectSpecialBankPacket`）。这些包是即时按钮（洗牌/填坑/买僵尸/升级
  点下即生效，没有"选中再确认"），为避免误触：单击朗读名称+费用
  （`GetCurrentPlantCost` 运行时真实值，内嵌表兜底）与状态（冷却中/
  阳光不足）；双击（同一包 450ms 内）才执行。执行经 Challenge 代理调
  宿主原生点击处理 `beghouled_packet_clicked`/`zombiquarium_packet_clicked`
  （与鼠标点按钮同一入口，自带扣费/冷却门禁），按执行前后阳光变化判成败
  播报"已执行/未能执行"。宿主语义 `SELECT_SEED` 对这些包不生效（实测）。
  包枚举经 objects 的 `seed_packets` 视图（与我是僵尸同一条安全路径，绕开
  `target_at(SELECT_SEED)`——宿主对 54+ 特殊包类型求名称/可用性越界，
  与老虎机 56/57 同族缺陷）；宿主崩溃守卫对 54..59 的命名同步走
  `SpecialBankPacketName`。
- **宝石迷阵：Ctrl+方向键拖动当前格植物**（`DragPlant` → `TAO_BOARD_SWAP`，
  target=该格植物句柄、column/row=目标格，宿主调 `Challenge::BeghouledSwap`）。
  植物句柄经 `GetTopPlantAt(TOPPLANT_ONLY_NORMAL_POSITION)` 取；成功后光标
  跟随被拖动的植物并朗读落点，交换未凑成匹配（宿主返回
  `TAO_ACTION_REQUIREMENT`，游戏会把植物弹回）播报"没有形成匹配"。
  裸回车只提示拖动方式（`BeghouledEnterHint`）。
- **宝石迷阵旋风：回车旋转当前格**（`RotateCurrent` → `TAO_BOARD_ROTATE`，
  column/row 为 2x2 区域左上角，宿主调 `Challenge::BeghouledTwistAt`）。
- **僵尸水族馆：回车投喂鱼食**（`FeedCurrent` → `TAO_BOARD_FEED`，扣 5
  阳光，column/row 换算像素落点）。
- **可达匹配提示音**（`MatchCue`，宝石迷阵/旋风）：浏览到某格时模拟该格四个
  方向的交换（旋风为当前格 2x2 顺时针旋转）能否凑成 3 连，可行即播
  `SOUND_BLEEP`（游戏内置音效，经 `tao.audio` 的 `resource_id` 直接引用，
  与 Lua 侧 `api.audio.play("SOUND_SWITCH")` 同机制）。同一格不重复响
  （F2 重读静默），离开再回来重播；交换/旋转后网格缓存作废重估
  （`matchCueGridDirty`）。判定用 5x5（旋风 6x6）局部类型快照
  （`FillBeghouledGrid`，`GetTopPlantAt(TOPPLANT_ONLY_NORMAL_POSITION)`），
  弹坑/空格不可参与匹配。
- **阳光金币自动收集**（`AutoCollectSunCoins`，与关卡奖励收集同一节流周期）：
  这些关卡没有向日葵，阳光全靠拾取场上掉落，水族馆阳光还落不进网格，
  手动定位对视障用户不可行；静默收集银币/金币/钻石/阳光类（CoinType 1..6），
  不播报。就绪提示（`UpdateCues`）对这三关同样跳过 `target_at(SELECT_SEED)`。

## 路标格子（观星 / 艺术锦标赛）

- 观星（`GAMEMODE_CHALLENGE_SEEING_STARS`=22）、坚果艺术锦标赛（36）、
  向日葵艺术锦标赛（40）中，棋盘上有一批发亮的路标格，要求种指定植物摆出图案
  （观星=阳桃阵、艺术锦标赛=坚果/向日葵图案）。格子朗读经
  `Challenge::GetArtChallengeSeed(col,row)`（语义 `get_art_seed`，非路标格返回
  `SEED_NONE`）在僵尸段之后追加"阳桃路标"段，整体形如
  "阳桃路标；草坪，B2"；SeedType 与 PlantType 编号一致，名称复用 `PlantDisplayName`。
  只在三种模式调用，其余关卡零开销；格子已种上普通位植物视为完成，不再提示路标
  （空南瓜罩仍提示）。

## 我是僵尸（I, Zombie）——宿主缺陷规避

我是僵尸关卡的"种子银行"里放的是**僵尸包**（`SeedPacket::mPacketType = SEED_ZOMBIE_*`，
类型 60+，远超 `NUM_SEED_TYPES`=53）。宿主对这类包求名称 / 可用性 / 阳光消耗
（`BoardTargetAt` / `BoardInvoke` 的 `SELECT_SEED` 分支 → `Plant::GetNameString`、
`GetCurrentPlantCost`、`PlantingRequirementsMet`）会**越界读植物定义表**，进程崩溃
（`0xC0000005`，与老虎机 `SEED_SLOT_MACHINE_*` 56/57 是同一宿主缺陷）。

实机现象：进入我是僵尸关卡、开场 Dave 对话结束进入棋盘后的**第一个 `UpdateCues`
tick** 即崩（每 tick 的"当前卡片就绪"提示会 `target_at(SELECT_SEED, seedIndex)`）。
规避：`BoardControl::IsIZombieLevel()`（`LawnApp::IsIZombieLevel`，失败回退按 `mGameMode`
61..70 判定）在本关卡**禁止一切语义 `SELECT_SEED` 路径**——就绪提示直接跳过、
数字键 / 循环键不选种子银行（播报"我是僵尸关卡暂不支持键盘选择僵尸"）。
**数字键选僵尸留待后续用原生点击实现**（宿主语义 `SELECT_SEED` 在此模式不可用）。

## 选卡界面

- 方向键按卡片/按钮的视觉位置做网格导航（不再是一维竖排循环）；
  左右方向在本行内移动，到达行首/行尾不跨行；上下方向没有上一行/下一行时
  同样不回退线性循环（均留在原位重读）。滑块控件仍用左右键调整数值。
- 卡片焦点朗读语序：限制前缀（不推荐/水生植物/夜间植物）+ 名称 +
  "已选入卡槽"（若已选）+ 阳光消耗 + 植物说明（tooltip，经
  `Plant.GetToolTip` 静态方法）。"不推荐"按原版判定：无水面选水生植物、
  白天选夜间植物。
- 模仿者对话框内的卡片同样支持网格导航与阳光/说明朗读。
- 选中/取消卡片的飞行动画期间，方向键仍从该植物的原网格位置移动，立即朗读目标，
  动画结束后同步宿主焦点；连续方向键以最后一次移动为准，不会被旧驻留重试拉回，
  也不会因暂时禁用全部卡片而跳到菜单。卡片暂不可枚举时仍按 SeedType 与钉住位置找起点。
  宿主 `ModAccessibilityUi.inl` 以 `mSeedsInFlight==0` 统一门控全部种子卡；
  `SeedChooserState.cpp` 按 SDK 真实字段读取飞行状态，飞行期保留动画前的可选状态，
  稳定期刷新禁用状态。宿主临时焦点事件不覆盖待同步的逻辑目的地；主动 Tab 导航会取消驻留。
  飞行期间方向键复用稳定候选缓存，驻留不扫描全树；只在面板停稳且戴夫对话结束后预采样，
  卡片落定后再解析当前节点并同步焦点。`tests/SeedChooser.cpp` 覆盖快速连续方向、节点重建、
  鼠标选卡、长动画、禁用卡、选满跳开始、弹窗/主动 Tab/离开界面取消，以及飞行期不全树扫描。
- 卡槽选满（含戴夫预选）后未选卡被宿主禁用（无 FOCUS 动作）：方向键评分
  跳过真正禁用的候选，飞行期临时禁用不按选满处理。
- 选卡完毕（无障碍树中已存在选卡、且所有未选卡均被宿主禁用，即
  `mSeedsInBank>=mNumPackets`）且卡片驻留完成后，焦点自动移交
  "一起摇滚吧！"；仅在该按钮可用（未被禁用/隐藏）时触发，按名称识别。
- Shift 朗读本关出怪列表（如"本关出怪：普通僵尸、路障僵尸……"）。名称跟随
  僵尸简称开关（F6），简称开启时去掉末尾"僵尸"二字。
- **出怪列表数据源**：宿主语义接口不暴露本关的僵尸集合（`Board::mZombieAllowed`
  是 DENY 字段、`mZombiesInWave` 是 2D 枚举数组、`NumberZombiesInWave` 只回数量）。
  但 `Challenge::InitZombieWaves` 在关卡初始化时会把本关出怪写进
  `Board::mZombieAllowed`（`bool[100]`，按 `ZombieType` 索引；反汇编确认
  `InitZombieWavesFromList` 即 `for t in list: board->mZombieAllowed[t]=1`）。
  故经宿主导出的 `TaoEngineResolveObject` 把 Board 句柄解成真实对象指针后直接读
  该数组（偏移 `0x5e30`，`BoardControl.hpp` 的 `kZombieAllowedOffset`）。这条路径
  对**所有模式**（冒险/小游戏/挑战/生存）都成立、无需逐个试玩；指针解析失败时回退
  冒险关静态表 `Board::CanZombieSpawnOnLevel`。偏移来自 20261005 版 exe，换版本
  需重新确认（`LogModeDiagnostics` 会在每个棋盘打印读到的集合，便于校验）。

## 疯狂戴夫的商店

所有入口共用 Store 的虚拟焦点：方向键浏览商品与按钮，Tab/Shift+Tab 只循环
上一页、返回（或主菜单）、下一页按钮，回车激活当前虚拟焦点。
从禅境花园/智慧树进入时，商店在弹窗打开事件内立即建立导航；原生开店过程的
嵌套更新帧也处理商店按键。购买确认框继续使用普通弹窗导航，关闭后恢复商店。

主菜单按 **F7** 打开商店（`tao.navigation` 的 `TAO_NAVIGATION_STORE`，未解锁时播报
"商店尚未解锁"）；键盘在主菜单"商店"按钮上回车也会改走同一非阻塞通道进店
（`SetBeforeActivate` 拦截，原生 ACTIVATE 的 exe 模态循环会阻塞整个开店期间、
冻结 mod 的全部分发与刷新）。商店由宿主以语义控件暴露，本 mod 的做法：

- **识别**：宿主把 `StoreScreen` 的 `widgets.describe` 类型上报为 `Dialog`，故按
  "子树里有商店商品子项（`item = 0x800000000 + StoreItem`）"来识别；`DIALOG_OPEN`
  事件节点作为锚点。
- **浏览（可用）**：方向键在"本页商品 + 上一页/下一页/主菜单"列表间移动，逐项朗读
  "序号/总数，名称，价格 N 金币"（价格取子项 `value`，金币不足补"，金币不足"）；
  进入商店/按 **F7** 会整页朗读"本页 N 件商品：…"。无名翻页按钮按 bounds 位置命名。
  进入商店时命名单行写日志："商店按钮命名：…"。
- **主菜单"商店"入口**：宿主不提供名称；首次经该按钮进店后按 bounds 学习并命名为
  "商店"（持久化到档案设置 `store_bounds_*`，本版实测坐标 (405,482,130x89)）。
- **购买（可用）**：商品语义子项被宿主标为 `DISABLED | BLOCKED`（`actions=0`），
  `ui.invoke` 一律拒绝。改为解析 `StoreScreen` 原生对象（`TaoEngineResolveObject`
  + vtable 校验）直调 exe：与原生 `MouseDown` 同序门禁（戴夫气泡→
  `AdvanceCrazyDaveDialog`、`CanInteractWithButtons`、`IsFullVersionOnly`→
  `IsItemSoldOut`→`IsItemUnavailable`→`IsComingSoon`→`CanAffordItem`，逐条中文
  播报拒绝原因），全过才调 `PurchaseItem`——它弹原生 [BUY_ITEM] 是/否确认框，
  确认框走普通弹窗焦点链路（是=购买，否=取消）。
- **退店（可用）**：退格 → `LawnApp::KillStoreScreen`（经 `StoreScreen+0x238` 的
  app 回指针）。这是 2026-10-09 的 PDB 定论：`ShowStoreScreen` 真身只是
  new+AddDialog（商店是对话框 id=4），无阻塞模态循环；`ButtonDepress(100)` 真身
  只写 `Dialog::mResult`（+0x22C）=1000 且全二进制无消费者按此关店——直调
  ButtonDepress、语义 ACTIVATE"主菜单"、`tao.navigation` GAME_SELECTOR 请求
  三条路实测都假成功（返回 OK 不关店），只有 KillStoreScreen
  （GetDialog(4)→KillDialog(4)→ClearUpdateBacklog）是鼠标点返回按钮后宿主
  销毁商店的同一入口。戴夫气泡在时退格先推进对话，再按一次才退店。

排障用 F12 的 DumpTree（会打印 `flags` 与命中探针）。

## 图鉴（Almanac）与僵尸包命名（宿主缺陷守卫）

订阅宿主 a11y 观测（`Narrator.Subscribe`）期间存在两类必然崩溃，`HostGuards.hpp`
用宿主自己的 `tao.interception` 机制拦截（宿主修复后可逐项移除）：

- **守卫 1（缺陷 D）**：打开图鉴时 `AlmanacDialog` 构造触发观测管线以**空棋盘指针**
  调 `Board::GetGridColumns`（0xC0000005；PDB 定位 `Plant::PlantInitialize` 插桩
  lambda +0x4ac）。拦截 `LawnApp::DoAlmanacDialog`：跳过原函数（图鉴不弹出），结果
  置空并播报原因。语义激活与原生鼠标点击两条路径都覆盖；语义激活另在
  `UiNavigation::ActivateFocused` 按名称拒绝（守卫安装失败时的兜底）。此前"点击
  图鉴即崩溃（ZDSR 环境冲突）"的归因不成立，与读屏组件无关。
- **守卫 2（缺陷 E）**：观测管线经 `BoardTargetAt` 为棋盘目标起名时调
  `Plant::GetNameString`，对僵尸包（60..74）等类型的 `%s` 参数是野指针（PDB 定位
  GetNameString 插桩 lambda +0x5d）。拦截 `Plant::GetNameString`：普通植物
  （0..52）走原实现，僵尸包/特殊包返回 mod 安全名（`BoardControl::ZombiePacketName`），
  不调 original。覆盖我是僵尸进关、暂停恢复等宿主重描述场景。

注意：宿主对 mod 更新回调有预算控制，一次超时会**永久禁用** OnUpdate（日志
"Disabled native update callback"），此时停订规避也随之失效；OnUpdate 内置慢段
诊断（超 5ms 记日志）用于定位耗时段。

## 禅境花园（Zen Garden）

GAMEMODE_CHALLENGE_ZEN_GARDEN（43，蘑菇园/水族园同一模式）。**模式判定只看
game mode==43，不看场地背景**——宿主 a11y 层对花园条目用同一判据
（ModAccessibilityUi.inl），自定义/常规关卡即使以蘑菇园/温室/水族馆作场地也走
常规键位（数字键选卡槽、Tab 冻结、退格放回卡片），不会误触花园键位；需求朗读
（zenCompose）也只在 mode 43 出现。宿主无障碍层已把
工具条、植物（value=需求描述）、臭臭、搬移落点暴露成语义项（ModAccessibilityUi.inl），
mod 的数字键/回车复刻宿主条目 activate 的**原生链路**（不经过 a11y 树枚举）：

- **数字键 1..8 选工具**（固定映射，顺序与宿主工具条一致）：可用性先查
  `Board::CanUseGameObject`（未购买/未解锁即拦截播报），拾取走
  `RefreshSeedPacketFromCursor` + `Board::PickUpTool`（与宿主工具条条目 activate
  同一链路），按光标是否变为对应 CursorType 确认成功。
- **回车使用工具**：对当前格植物按工具分派——浇水壶/肥料/杀虫剂/留声机/巧克力走
  `ZenGarden::FeedPlant`（坐标与鼠标路径一致，需求不符先核对拒绝并播报它真正需要什么；
  巧克力门禁 `PlantCanHaveChocolate`）；手套/卖植物/手推车走
  `ZenGarden::UseToolOnPlant`；施用后重读需求播报"接下来需要 X"。
  搬移态（光标 PLANT_FROM_GLOVE / PLANT_FROM_WHEEL_BARROW）回车 = 放到当前格
  （`MovePlant` / `PlaceWheelbarrowPlant` + `ClearCursor`，落点须为空格），手套搬移的
  植物句柄由 mod 记录（宿主只暴露光标类型，不暴露 mGlovePlantID）。
- **需求状态朗读**：格子朗读时植物名后追加"，需要浇水/施肥/杀虫/放音乐"或
  "，无需照料"（`GetDynamicPlantNeed` / `GetPlantsNeed(PottedPlantFromIndex)`，
  与宿主植物条目同源）；Shift 播报花园总览（逐格统计各需求数量 + 臭臭醒睡状态）。
- **退格放回工具/搬移中的植物**：`Board::ClearCursor`（宿主植物条目收尾同款），
  成功不播报；**空手时退格 = 返回主菜单**（`Board::ActivateMenuButton`——花园"菜单"
  按钮的原生终点即 DoBackToMain；`ZenGarden::LeaveGarden` 不是这条路，实测无效。
  开局前 ~10 秒菜单按钮解锁倒计时内会静默失败，与原生按钮一致）。
- **Tab（及 F7）打开疯狂戴夫的商店**：`ZenGarden::OpenStore`（花园商店按钮的原生终点），
  花园里冻结游戏没意义；开店后店内导航由 Store 的弹窗锚点自动接管。
  店内 Tab 循环导航按钮；暂停菜单中仍走控件导航。
- **数字键 0 前往下一个花园**：`ZenGarden::GotoNextGarden`（宿主"下一花园"按钮条目的
  原生终点，门禁 `CanUseGameObject(OBJECT_TYPE_NEXT_GARDEN)` 同款——未解锁更多花园时
  播报"没有更多花园"），切换成功播报新花园名。
- 守卫：禅境花园禁铲（铲盆栽=丢弃收集植物，指引用数字键 7 卖植物）；就绪提示跳过
  `target_at(SELECT_SEED)`（花园无种子银行，空枚举与老虎机/我是僵尸同族崩溃风险）；
  减号/等号循环选卡在花园改为提示数字键选工具。

实机验证（2026-10-09）：浇水/需求播报/未解锁工具拦截/搬移放回全部通过；
"已浇水，接下来需要浇水"为幼苗原生机制（需连续浇水才长成），播报准确。

## 老虎机（Slot Machine）关卡（实机排障结论）

老虎机关卡在宿主侧有几个必然崩溃点，mod 侧据此规避（全部只用语义 ABI，不碰内存/字节补丁）：

- **对话框/菜单导航崩溃**：存档恢复的棋盘在控件树里是一个 `Board` 控件，宿主
  `ui.describe` 它时会调 `BoardActionGate` 读悬垂状态 → 0xC0000005。注意
  `context.board` 是 Board 的**对象句柄**，与控件树里的 **Widget 句柄**不同命名空间，
  不能用句柄比较。`UiNavigation` 在老虎机模式下用 `widgets.describe` 的 `type_name`
  识别 `Board` 控件并整棵跳过（`SetSkipBoardWidget`），续玩对话框/菜单即可正常导航。
- **拉杆后宿主观测管线崩溃**：只要存在 a11y 订阅读者，宿主每 tick 全量计算
  `board->mAdvice`，拉杆/滚动帧解引用悬垂指针崩溃（读者归零是唯一规避手段）。
  实测拉杆前订阅正常（续玩对话框、暂停菜单可播报），拉杆后即使人在暂停菜单里
  恢复订阅也会崩。故以 `Challenge::mSlotMachineRollCount > 0`（已拉过杆）为界，
  整段停订（`narrator.Unsubscribe`）至离开关卡；停订期间的弹窗由 10/s 采样自查
  模态上升沿补播标题。导航光标不受影响（走 `widgets`/`ui` 接口，与订阅无关）。
- **拉杆走原生点击**：语义 `TAO_BOARD_SLOT_PULL` 返回成功但跳过原生点击链路的
  提示/状态切换，约 1s 后崩溃；改走 `Challenge::SlotMachineGetHandleRect` 的原生
  `Board::MouseDown`（与鼠标同一入口）。
- **掉落卡拾取**：不走 `target_at(SELECT_CARD)`（无卡可枚举时崩溃），改 objects
  `coins` 视图自行枚举 `COIN_USABLE_SEED_PACKET`；名称取 `Coin::mUsableSeedType`
  （`GetUsableSeedType()`，与宿主命名一致，勿用 `GetFinalSeedPacketType`——老虎机
  掉卡时它可能返回非植物类型 56/57 → 念成"未知植物"）。
- **自动收集**：老虎机关卡照常收集（否则通关奖励不领），但每次拉杆后暂缓 ~5s
  （避开拉杆后 coins 视图枚举的崩溃窗口）。
- **就绪提示**跳过 `target_at(SELECT_SEED)`（宿主对 `SEED_SLOT_MACHINE_*` 越界读）。
- **旋转结果播报**：底部建议文本（`Board::mAdvice`，实为 `MessageWidget`）在 SDK 里
  没有可读 getter（该 proxy 只有 `set_label`，`mLabel` 是 char 数组；`SetLabel` 也不在
  类型化拦截清单里），故结果由场上掉落的可用种子包合成（"N 株植物：…" /
  "没有新植物，再拉一次"）；底部建议栏显示上升沿或从弹窗返回时补播一次。
