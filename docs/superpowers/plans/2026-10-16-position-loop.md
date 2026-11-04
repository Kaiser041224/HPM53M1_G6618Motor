# 位置环（5kHz）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 GPTMR1 CH0 5kHz 拍上实现 ODrive 级联位置模式（二阶滤波 + 位置 P → 现有速度环 PI + 摩擦前馈 + 力矩前馈）。

**Architecture:** 位置环挂 CH0 独立拍（新增 `app_motion_timer_bind_position`），角度/速度反馈复用 10kHz 速度拍估计器产物（CH0 零 SPI）；两回调经共享 IRQ 天然串行，`vel_cmd` 单 float 交接。ODrive 源码级对齐：滤波增益 `kp_f=bw²`、`ki_f=2bw`，`vel_des = vel_sp + pos_gain·pos_err`，速度环复用 `algo_motion_speed` + `algo_friction_torque(vel_cmd)` + `torque_ff`。

**Tech Stack:** C17 / HPM53M1 / GPTMR1 双拍 / 宿主测试（scripts/tests/control）

**Spec:** `docs/superpowers/specs/2026-10-16-position-loop-design.md`

## Global Constraints

- CH0 拍零 SPI/RTT/flash 调用；`ATTR_RAMFUNC`；有界执行。
- 增益禁写时机：`s_motion.active` 时拒绝（与 `app_motion_gains` 同纪律）；flash save 仍需 FOC OFF。
- 数值/文案/条件改动必须配行为钉 + 反向验证（mutation：恢复 bug 确认测试变红）；python 替换后必须 grep 确认落地（本会话多次静默脱靶）。
- 命名禁止缩写；版权头/Doxygen 沿用工程规范；`clang-format -i` 后复跑测试。
- 参数消费走 `app_software_params_current()` 读、`_mutable()` 写透；schema/YAML/fixture 三处同步；新字段进 `APPLY_REBOOT`（init 期缓存）。

## Review Focus

1. **滤波带宽钳位**：`bw > 0.25/dt`（5kHz→1250 rad/s）不越界发散 → Task 1 钳位测试。
2. **多圈连续位置在 wrap 边界/zero/apply 处的连续性与坐标系重置** → Task 2 unwrap/重置测试。
3. **`vel_ff` 双重计入**：input_vel 已在滤波器内进 vel_setpoint，vel_cmd 不得再加 → Task 2 前馈测试。
4. **活动中切模式/改增益被拒**（MODE_LOCKED / FOC_NOT_READY 分类不变）→ Task 2 命令分类测试。
5. **2:1 拍交织下 vel_cmd 交接**（CH0 写 CH1 读，mock 交错驱动）→ Task 2 级联收敛测试。

---

### Task 1: 算法层 — ODrive POS_FILTER + 位置 P（`algo_motion`）

**Files:**
- Modify: `App/Algorithm/Inc/algo_motion.h`
- Modify: `App/Algorithm/Src/algo_motion.c`
- Test: `scripts/tests/control/test_motion_math.c`

**Interfaces:**
- Produces（后续任务依赖，签名固定）:
  - `typedef struct { float pos_setpoint_rad, vel_setpoint_rad_s; } algo_motion_pos_filter_t;`
  - `void algo_motion_pos_filter_step(algo_motion_pos_filter_t* f, float input_pos, float input_vel, float bandwidth_rad_s, float dt);` — ODrive POS_FILTER 单步：`accel = kp_f·(input_pos−pos_sp) + ki_f·(input_vel−vel_sp)`，`vel_sp += dt·accel`，`pos_sp += dt·vel_sp`；内部 `ki_f = 2·bw`、`kp_f = 0.25·ki_f²`，`bw` 钳位 ≤ `0.25f/dt`；`bw ≤ 0` 时滤波器直通（`pos_sp=input_pos, vel_sp=input_vel`）。
  - `float algo_motion_position(float pos_setpoint, float pos_est, float pos_gain, float vel_ff);` — 返回 `vel_ff + pos_gain·(pos_setpoint − pos_est)`。

[x] **Step 1: 写失败测试**（test_motion_math.c，函数名+断言）：
  - `test_motion_pos_filter()`：bw=10、dt=2e-4，阶跃 input_pos 0→1（input_vel=0）：(a) 任意拍 `pos_sp ≤ 1.0001`（无超调，临界阻尼）；(b) 0.6s 后 `|1−pos_sp| < 0.01`；(c) 稳态 `|vel_sp| < 0.01`。再验 input_vel=0.5 阶跃后 vel_sp→0.5、pos_sp 斜坡趋同。
  - `test_motion_pos_filter_bw_clamp()`：bw=2000、dt=2e-4（>1250）不发散：跑 0.5s 后 pos_sp 有限且 |pos_sp−1|<1（钳位生效）。
  - `test_motion_position_p()`：`algo_motion_position(1, 0.5, 20, 0.3) == 10.3±1e-5`；`pos_gain=0` 时返回 vel_ff。
[x] **Step 2: 跑测确认失败**：`bash scripts/tests/control/run.sh` → FAIL（符号未定义）。
[x] **Step 3: 实现**（签名见上；滤波增益在函数内推导，注释引 ODrive `update_filter_gains`）。
[x] **Step 4: 跑测确认通过**：`bash scripts/tests/control/run.sh` → 0 failures。
[x] **Step 5: mutation**：把 `kp_f = 0.25f*ki_f*ki_f` 改回 `2*bw`（非临界阻尼）→ 无超调/收敛测试须红；恢复后绿。

### Task 2: app_motion 位置模式（API + 5kHz 拍 + 连续位置）

**Files:**
- Modify: `App/Control/Inc/app_motion.h`
- Modify: `App/Control/Src/app_motion.c`
- Test: `scripts/tests/control/test_motion.c`

**Interfaces:**
- Consumes: Task 1 全部。
- Produces:
  - `typedef enum { APP_MOTION_MODE_TORQUE = 0, APP_MOTION_MODE_SPEED, APP_MOTION_MODE_POSITION } app_motion_mode_t;`（替代内部 `bool s_speed_mode`）
  - `int app_motion_position(float position_rad, float vel_ff_rad_s, float torque_ff_nm);`
  - `int app_motion_position_gains(float pos_gain, float filter_bandwidth_rad_s);`（idle-only，0<gain≤100、0<bw≤100）
  - `void app_motion_position_step(void);`（`ATTR_RAMFUNC`，5kHz CH0 拍入口）
  - snapshot 增 `app_motion_mode_t mode; float position_rad, pos_setpoint_rad, vel_cmd_rad_s;`
  - 命令返回码复用 `app_motion_cmd_status_t`（新增语义：MODE_LOCKED 涵盖三模式切换）。
- 行为钉要点：
  - `app_motion_step` 在 `shaft_rad` 更新后做 wrap-delta 累积 `position_rad`；`init`/`zero`/`apply` 归零。
  - POSITION 分支：`torque = algo_motion_speed(pi, s_vel_cmd_rad_s, speed, kp, ki, dt, limit) + (suspended?0:algo_friction_torque(s_vel_cmd_rad_s)) + torque_ff`，按 torque_limit 钳位。
  - `app_motion_position_step`：`!active || mode!=POSITION || !feedback_valid` 早退；滤波 → `vel_cmd = algo_motion_position(pos_sp, position_rad, pos_gain, vel_ff)`；`motion_speed_limit_rad_s>0` 时钳位 vel_cmd。
  - `app_motion_position` 命令链与 `motion_command` 同（校验/授权/epoch），追加：`|vel_ff|` 限 `motion_speed_limit`（>0 时）、`|torque_ff|` 限 `reducer·Kt·iq_max`、`pos_gain≤0 → GAIN_NOT_SET`；活动中同模式重发=目标更新。

[x] **Step 1: 写失败测试**（test_motion.c）：
  - `test_motion_position_command()`：分类——无增益→GAIN_NOT_SET；idle 切 MODE_LOCKED（活动中发 `motion_speed`/`app_motion_torque` 互斥）；vel_ff 超限→TARGET_LIMIT；torque_ff 超限→TARGET_LIMIT；活动中重发 position=0 成功。
  - `test_motion_position_gains()`：范围外（0、100.1、bw=0）→BAD_PARAM；active 中→FOC_NOT_READY；成功后 snapshot/参数单例同步。
  - `test_motion_position_cascade()`：mock 植物（速度=力矩/负载系数，转子随速度推进 + 原码跟随），`app_motion_position(θ,0,0)` 后每 2 拍 `app_motion_position_step`、每拍 `app_motion_step`，收敛 `|position_rad−θ|<0.02` 且 `|vel_cmd|<0.5`（2:1 交织 = Task 5 关注点 5）。
  - `test_motion_position_multiturn()`：植物转过 ≥3 圈跨 wrap 边界，`position_rad` 连续单调（相邻差 <π）；`app_motion_zero()` 后归零。
  - `test_motion_position_ff()`：vel_ff=0.3 时稳态 `vel_cmd≈0.3`（不再重复加）；torque_ff=0.2 时稳态 `torque≈0.2+摩擦(vel_cmd)`（前馈叠加）。
[x] **Step 2: 跑测确认失败**（编译错/分类断言红）。
[x] **Step 3: 实现**（顺序：头文件枚举与 API → `s_speed_mode`→`s_mode` 改名贯通 → 连续位置累积 → position_step → POSITION 分支 → gains/position 命令）。
[x] **Step 4: 跑测确认通过**（control 全绿）。
[x] **Step 5: mutation**：(a) `position_rad` 不累积（恒 0）→ cascade/multiturn 红；(b) vel_cmd 再加 vel_ff → ff 测试红。

### Task 3: 定时器绑定 + 接线（bind_position）

**Files:**
- Modify: `App/Platform/Inc/app_motion_timer.h`
- Modify: `App/Platform/Src/app_motion_timer.c`
- Modify: `App/Logic/app_logic.c`
- Test: `scripts/tests/control/test_motion_timer.c`

**Interfaces:**
- Consumes: `app_motion_position_step`（Task 2）。
- Produces: `void app_motion_timer_bind_position(void (*callback)(void));`（停止期间绑定；回调有界）。
- 行为：`motion_timer_position` 内 observe 后调 position 回调并计 `execution_max_cycles`（镜像 velocity 拍）。

[x] **Step 1: 写失败测试**：bind_position 回调被 position 拍调用、execution_max 更新（镜像现有 velocity 绑定测试形态）。
[x] **Step 2: 跑测确认失败**。
[x] **Step 3: 实现** + `app_logic.c` 绑定 `app_motion_position_step`（紧邻 `bind_velocity` 行）。
[x] **Step 4: 跑测确认通过**（control + 构建）。

### Task 4: 参数管线（schema + YAML + fixture）

**Files:**
- Modify: `scripts/gen_params.py`（SCHEMA software 段 + `APPLY_REBOOT` 两键）
- Modify: `config/software.yaml`（`control.position_loop:` 新段）
- Modify: `scripts/tests/fixtures/software.yaml`（同字段）

**Interfaces:**
- Produces: `software.control.position_loop.pos_gain`（出厂 20.0，[1/s]，范围 [0,100]）、
  `software.control.position_loop.input_filter_bandwidth`（出厂 10.0，[rad/s]，范围 [0,100]）→ 生成 `app_software_params_t.control.position_loop.{pos_gain,input_filter_bandwidth}`。

[x] **Step 1: 写失败测试**：`scripts/tests/fixtures/software.yaml` 加字段后 `python3 scripts/test_gen_params.py` 断言生成结构含新键（沿用现有用例模式新增一条）。
[x] **Step 2: 跑测确认失败**。
[x] **Step 3: 实现**（schema 两行 `(f32, 0.0, 100.0)` + APPLY_REBOOT 两键 + YAML 段 + fixture）。
[x] **Step 4: 跑测确认通过**（`python3 scripts/test_gen_params.py` 全绿）。

### Task 5: 终端命令（motion pos / motion pos_gains / show）

**Files:**
- Modify: `App/Comm/terminal/Src/app_terminal_cmd_foc.c`

**Interfaces:**
- Consumes: `app_motion_position` / `app_motion_position_gains` / snapshot 新字段。
- Produce: `motion pos <rad> [vel_ff_rad_s] [torque_ff_nm]`；`motion pos_gains <pos_gain> [input_filter_bandwidth]`（idle 提示与 gains 一致）；`motion show` 增 mode/position/pos_setpoint/vel_cmd；`app_terminal_motion_reason` 的 MODE_LOCKED 文案改 "torque/speed/position switch while active"；usage 字符串同步。

[x] **Step 1: 实现命令解析**（strtof + 非有限值→BAD_PARAM 处置，镜像 `motion speed` 形态）。
[x] **Step 2: 构建 + 手验路径**：`make artifacts` 无警告；`motion` usage 输出含新行（构建期 grep 源码即断言 usage 串存在，回归靠 control 测试不变红）。
[x] **Step 3: mutation**：usage/提示文案断言（grep 钉：MODE_LOCKED 文案含 "position"）。

### Task 6: 收尾（回归 + 构建 + 热路径 + 暂存）

[x] **Step 1:** `clang-format -i` 改动文件后复跑 `python3 scripts/test_gen_params.py` + 五套宿主（encoder/foc/control/identify/params）两配置 + UBSan（encoder/control）。
[x] **Step 2:** `make artifacts` 无警告；`app_motion_step`/`app_motion_position_step` 反汇编无 Flash 调用（jal/jalr 目标不在 ILM 外）；`git diff HEAD --check`。
[x] **Step 3:** 设计文档 §8 测试项对照打勾；`git add -u`；报告改动文件/原因/风险/上板验证步骤（不 commit，等用户指令）。
