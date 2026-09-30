# Gui 界面重妆实现计划（TNO 硅晶之梦 · 计划 3）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `Gui/ref/` 设计系统把 `Gui/app` 从 Material 默认外观重妆为「TNO 硅晶之梦 · 复古未来主义」：tokens/字体/发丝线框/辉光/单页平铺布局/三态数据语义，产出可运行 release exe 供 Kaiser 验收观感。

**Architecture:** 规范源 = `Gui/ref/design-system/design-system.md`（下称 DS）+ `tokens.css` + `templates/gui-main.html` + `previews/gui-main.png`（下称 PREVIEW）——**本计划不复述其内容，实现逐条对照**。落地：`tokens.css` → Dart `GuiPalette/GuiTypeScale/GuiGlowStyles`（ThemeExtension）→ `lib/widgets/` 母题组件 → 单页平铺重组 `lib/ui/`。只动显示层（`Gui/app/lib/**`、`assets/`、`test/`），禁改 `Gui/core|frb|sim|protocol`、`Gui/ref/`、固件目录。

**Tech Stack:** Flutter（Material 仅作骨架，视觉全面自绘）、Dart、OFL 字体（Electrolize/Aldrich 打包；Cascadia Mono/Noto Sans SC 系统字体+回退）。

**已批准的实现设计（Kaiser 2026-09-30，6 节全通过）：** 范围=A1~A5+DS §4 布局重排；曲线区框架占位（波形属 B 阶段），热与负载分段条/FREQ-MOD LED 条/相电流占比实装；tokens 零硬编码；三态语义；验收=DS §8 清单 13 条。

## Global Constraints

- **DS §8 应用检查清单 = 验收标准**（逐条过）。三条不可让步：颜色不单独承载语义（色+图标+文字）；`0` 与 `—` 区分（stale 降级）；状态辉光不串色。
- 直角（radius 0）、发丝线恒 1px「一亮两暗」、醒目分割线全页 ≤3 条、标题全大写+字距 0.16em、数值等宽 tabular 右对齐、文字半透明 rgba。
- 页面代码**零硬编码色值**（全部取自 `GuiPalette`/`GuiGlowStyles`）；色值以 `tokens.css` 为准（DS 记录其为实测值）。
- 字体授权（`ref/design-system/font-licensing.md`）：Electrolize/Aldrich（OFL，从 `ref/design-system/fonts/` 复制到 `Gui/app/assets/fonts/` 并打包）；**方正系/黄油体禁用**；中文 Noto Sans SC（回退 Microsoft YaHei UI）。
- 数值精度按 DS §5：电流 3 位、母线 1 位、频率 2 位（量纲决定，不用全局常量）。
- 设备 ASCII 标识字形/色序**禁手抄**：以 `ref/templates/gui-main.html` 内嵌数据（或 `tools/extract-device-art.ps1` 输出）为准固化 Dart 常量；色序 45/51/141/207/213 → `#00D7FF/#00FFFF/#AF87FF/#FF5FFF/#FF87FF`。
- **诚实显示**：协议未提供的量（如设备 SEQ）显示 `—`，不得估算冒充；UPTIME=连接会话时长（Dart 侧计时）、RTT=STATUS_GET 往返实测、帧数=stats.attempts。
- 回归门：`flutter analyze` 干净、`flutter test` ≥14 绿（既有测试随重排迁移语义，断言可适配但意图保留）、`flutter build windows --release` 成功、`cargo test --workspace` 47 不回归。
- 每任务禁 git（控制器提交）。

## File Structure（新增/重写）

```text
Gui/app/
├── assets/fonts/{Electrolize-Regular.ttf, Aldrich-Regular.ttf}   # 从 ref/design-system/fonts/ 复制
├── pubspec.yaml                      # + flutter: fonts 段 + assets
└── lib/
    ├── theme/
    │   ├── gui_palette.dart          # GuiPalette（基底/线/文字/状态/系列色）+ ThemeExtension
    │   ├── gui_type.dart             # GuiTypeScale（字阶字距）+ GuiGlowStyles（辉光层级）
    │   └── gui_theme.dart            # ThemeData 组装（深色、直角、字体族）
    ├── widgets/
    │   ├── hairline_frame.dart       # 一亮两暗线框 / 分割线（HairlineDivider）
    │   ├── section_title.dart        # 全大写区块标题
    │   ├── glow_value.dart           # GlowText（强/弱/极弱辉光+描边）/ GlowValue（数值+单位）
    │   ├── segment_bar.dart          # LED 分段条（9px/2px）
    │   ├── status_badge.dart         # 三重编码徽标（normal/warn/fault/latched/stale/pending）
    │   ├── stale_gate.dart           # 三态包装（live/stale/unknown）
    │   └── device_ascii.dart         # 设备 ASCII 标识 + device_art_data.dart（常量）
    ├── ui/
    │   ├── single_page.dart          # 单页平铺骨架（替代 shell.dart 三页）
    │   ├── top_bar.dart              # 连接选项卡（展开面板）+ 设备标识区 + 右侧遥测
    │   ├── status_zone.dart          # 8 瓦片 + 热与负载分段条 + 控制条 + 故障构成
    │   ├── scope_zone.dart           # 曲线占位框架（一级流预留）
    │   ├── param_zone.dart           # 参数区（双栏分组表 + 联锁提示行）
    │   └── bottom_bar.dart           # DEGRADED/计数/帧
    └── state/app_state.dart          # + 三态时间戳 + rtt/uptime + 阈值访问
```

旧 `shell.dart/connect_page.dart/param_page.dart/dashboard_page.dart` 的**内容**迁入新组件（编辑对话框/联锁/COMMIT 交互全保留），迁移完成后删除旧文件；`status_bar.dart` 并入 `bottom_bar.dart`。

---

### Task 1: tokens + 字体 + 基础母题组件

**Files:** Create `lib/theme/**`、`lib/widgets/{hairline_frame,section_title,glow_value}.dart`、`assets/fonts/**`；Modify `pubspec.yaml`、`lib/main.dart`（换 ThemeData）；Test `test/theme_test.dart`。

**Interfaces（后续任务依赖）:**
- `GuiPalette.of(context)` → `bgVoid/bgBase/bgPanel/bgRaise/bgHover/bgActive, lineHi/line/lineDim, fg/fgStrong/fgDim/fgMute, accent/accent2, stNormal/stWarn/stFault/stLatched/stStale/stPending, series1..6`
- `GuiTypeScale.of(context)` → `title/section/label/data/body`（TextStyle，含字距/字体族）
- `GuiGlowStyles` → `strong(TextStyle 含 shadows+stroke)/soft/wide/none` 与 `status(Color)`→同色辉光
- `HairlineFrame({child, variant: main|sub})`；`HairlineDivider()`（一亮两暗）
- `SectionTitle({title, en, trailing})`（样式 = DS `.gui-section-title`）
- `GlowText({text, strength: strong|soft|weak})`；`GlowValue({value, unit, precision})`（tabular 右对齐，precision 按量纲传入）

- [ ] **Step 1:** 复制 `ref/design-system/fonts/*.ttf` → `Gui/app/assets/fonts/`；`pubspec.yaml` 注册字体族（Electrolize/Aldrich）+ assets。
- [ ] **Step 2:** 按 `tokens.css` 逐值实现 `gui_palette.dart`/`gui_type.dart`（色值 hex 原样照抄 tokens.css——那是实测值）；`gui_theme.dart` 组装 ThemeData（scaffoldBackgroundColor=bgBase、直角全局、SelectionArea 等）。
- [ ] **Step 3:** 失败测试先行（`test/theme_test.dart`）：`buildGuiTheme()` 断言 palette 值 == tokens.css 锚点（bgBase=#081018、accent=#58C8C0、accent2=#C8C000、cornerRadius=0）；GlowText 渲染含 3 层 shadows。
- [ ] **Step 4:** 实现三组件（发丝线框用 Stack+三重 1px Container 或 BoxDecoration+boxShadow 三层，色取 palette；辉光 = Text.style.shadows 四层按等级取舍 + stroke 用 foreground Paint）。
- [ ] **Step 5:** `flutter test` 绿 + `flutter build windows --debug` 过 + Commit（控制器）。

### Task 2: 语义组件 + DeviceAscii

**Files:** Create `lib/widgets/{segment_bar,status_badge,stale_gate,device_ascii,device_art_data}.dart`；Test `test/widgets_test.dart`。

**Interfaces:**
- `SegmentBar({value01, segments=24, onColor, warnThreshold})`（9px 段/2px 隙，`.on` 段发光）
- `StatusBadge({state: normal|warn|fault|latched|stale|pending, label})`——**色+图标+文字**（图标：✓/!/✖/⚠/∅/…，文字标签必带）
- `StaleGate({state: live|stale|unknown, child, onStale})`——stale 时 child 换 `—` 无辉光虚线框
- `DeviceAscii({connected, modelName})`——38×5 块字符五段渐变（数据从 `device_art_data.dart` 常量读取，按列映射 色1..5）；未连接显示 `DEVICE ID PENDING` 占位框
- `device_art_data.dart`：`artLines: List<String>`（5 行）+ `colorStops: List<Color>`（5 色）——**从 `ref/templates/gui-main.html` 提取**（grep 块字符行与色序注释；如需可用 `ref/tools/extract-device-art.ps1` 复核，禁手抄色号）

- [ ] **Step 1:** 测试先行：SegmentBar 段数/亮段数断言；StatusBadge 三重编码（含图标与文字）；StaleGate 三态切换。
- [ ] **Step 2:** 提取 device art 数据（写明来源文件与行号进报告）。
- [ ] **Step 3:** 实现四组件 → 测试绿 → build 过 → Commit。

### Task 3: 单页骨架 + 顶栏（含连接选项卡展开）

**Files:** Create `lib/ui/{single_page,top_bar}.dart`；Modify `lib/main.dart`、`lib/state/app_state.dart`（+rtt/uptime/三态字段骨架）；Delete 旧 `shell.dart`（内容分流后）；Test 迁移 `widget_test.dart`。

**Interfaces:**
- `SinglePage(state)`：Column = TopBar → Expanded(SingleChildScrollView: StatusZone → ScopeZone → ParamZone) → BottomBar（本任务先占位后三者）
- `TopBar(state)`：左「连接·TCP」选项卡（点击展开详情面板：链路选择三段（TCP 可用/USB BULK 禁用/UART·CDC 禁用）、地址输入、连接/断开、HANDSHAKE 行（PROTOCOL v1.0 / CAPABILITIES / PARAM META 计数）、LINK QUALITY 行（RTT/CRC/重连））；中 `DeviceAscii` + 型号/MCU/FW 行（**来自 protoInfo**，未连接 `DEVICE ID PENDING`）；右 POLL/UPTIME/SEQ/帧（诚实显示规则见 Global Constraints）。展开面板 `Overlay` 或 Stack+Positioned——**相对顶栏定位，不遮设备标识**（DS §4.2）。
- 旧 ConnectPage 的连接逻辑并入展开面板；测试构造器 `AppState.test` 保持兼容。

- [ ] **Step 1:** 测试（未连接顶栏显示 DEVICE ID PENDING；连接后显示 protoInfo 名；点选项卡展开面板含 TRANSPORT/ADDRESS 字样）。
- [ ] **Step 2:** 实现 + main 切换到 SinglePage → 旧 shell/connect_page 删除。
- [ ] **Step 3:** flutter analyze/test/build 过（既有 14 测试中 UI 布局类断言按新结构迁移，语义不变）→ Commit。

### Task 4: 状态监视区（8 瓦片 + 热与负载 + 控制条 + 故障构成）

**Files:** Create `lib/ui/status_zone.dart`（替代 dashboard_page 主体）；Modify `app_state.dart`（阈值访问：从已加载 params 取 `software.fault.ntc_ot_c`/`vbus_ov_v` 等，缺省显示 `—`）；Test `test/status_zone_test.dart`。

**要点（对照 PREVIEW）:**
- 8 瓦片（VBUS/I_U/I_V/I_W/NTC/ENC R/ENC O/F_ELEC）：`GlowValue` + 精度（V 1 位/A 3 位/Hz 2 位/角度 1 位）+ 子注释（NOMINAL/RMS/RROTOR/OUTPUT/SETPOINT）+ 三态（stale → `—` + dashed 边 + 「N 秒未更新」黄字，warn → 黄值+「接近门限」）。
- 热与负载·相对门限：6 条（NTC/U/V/W/VBUS/模组）水平条（值/门限 %），门限来自 params（无门限 → 条灰显 `—`）。
- 控制条：启动/停止/清除故障（保留联锁 SnackBar 文案表）+ FREQ/MOD `SegmentBar` + 右侧数值。
- 故障构成：flags≠0 时按位出条目（位语义表从 `App/Control/app_fault.h` **只读提取**；无法解码的位显示 `bit N (0x..)`）。
- [ ] TDD：瓦片三态/stale 断言、精度格式断言、联锁文案保留断言 → 实现 → 迁移 dashboard 测试 → 全绿 → Commit。

### Task 5: 参数区（双栏分组表 + 联锁提示行）

**Files:** Create `lib/ui/param_zone.dart`（吸收旧 param_page 交互）；Test 迁移 `param_page_test.dart`→`param_zone_test.dart`。

**要点（对照 PREVIEW 参数区）:** 搜索框 + 读全部/COMMIT 落盘（二次确认保留）；双栏分组（Wrap/Row 两列，每组 SectionTitle「MOTOR · N 项」）；行 = 名称(参数名等宽) | 类型 | 范围 | LIVE/REBOOT 徽标 | 值（右对齐强辉光）；dirty 行圆点+黄值；编辑交互（对话框或内联）与范围校验/错误码文案**原样保留**；底部联锁提示行（黄字实况：「落盘需停机且无故障锁存——当前 X，锁存 N 项，COMMIT 将被拒绝」，允许时为绿字）。
- [ ] TDD：迁移 2 个既有测试（分组生成/越界拒绝）+ 新增 dirty 视觉与联锁提示行断言 → 实现 → 全绿 → Commit。

### Task 6: 数据语义三态 + 底栏 + 遥测

**Files:** Modify `app_state.dart`（每值时间戳 Map、stale 判定 >1s、rtt 实测（statusGet 计时 EMA）、uptime（连接时刻起）、dirty 计数导出）；Create `lib/ui/bottom_bar.dart`（替代 status_bar.dart）；Test `test/app_state_test.dart` 扩展。

**要点:** 底栏 = 状态徽标（DEGRADED/READY/未连接，三重编码）+ RTT/CRC/RESYNC/RETRY/DIRTY + SEQ `—`/帧 attempts（诚实显示）；stale 判定单点实现 `ValueFreshness`。
- [ ] TDD：三态降级测试（注入旧时间戳→stale→`—`）；rtt/uptime 字段存在性 → 实现 → 全绿 → Commit。

### Task 7: 回归 + DS §8 清单验收 + release

- [ ] `flutter analyze`/`flutter test`/`flutter build windows --release` 全过；`cargo test --workspace` 47 不回归。
- [ ] **DS §8 十三条逐条自检**（写入报告：每条 ✓/✗ + 证据行号）。
- [ ] 与 `previews/gui-main.png` 对照差距清单（如实列出未达成项与原因）。
- [ ] Commit + 交 Kaiser 试运行。

---

## Self-Review 记录

1. **覆盖**：DS §2 色板→T1；§2.3/2.4 文字/辉光→T1；§3 结构母题→T1/T2；§4 布局→T3/T4/T5；§5 字体排版→T1/T4（精度）；§6 状态语义/三规则→T2/T4/T6；§8 清单→T7。PREVIEW 的顶栏/瓦片/控制条/参数区/底栏/对照 A·B → T3~T6。
2. **占位符**：无 TBD；「从 html/脚本提取」为具名来源的提取指令（有产出要求）。
3. **类型一致性**：GuiPalette/GuiTypeScale/GuiGlowStyles/HairlineFrame/SectionTitle/GlowText/GlowValue/SegmentBar/StatusBadge/StaleGate/DeviceAscii 在 T1/T2 定义、T3~T6 引用，签名如上。
