# 位置环（5kHz）设计 — ODrive 级联位置模式

- 日期：2026-10-16
- 基线：`ac6f9e4` 之后（摩擦前馈全量已提交）
- 上游文档：`2026-10-13-motion-foundation.md`（CH0/CH1 双拍预留）、`2026-09-21-foc-algorithm-design.md` §8.4（外环分频预留）、`2026-10-15-friction-feedforward-design.md`（速度环现状）

## 1. 目标

- 在预留的 **GPTMR1 CH0 5kHz 独立拍**上实现位置环，位置控制模式参照 ODrive 控制器结构（源码级对齐）。
- 复用速度环 10kHz 估计器产物（轴角/速度），**CH0 拍零 SPI 访问**（用户约束）。
- 终端 `motion pos` 命令 + `motion pos_gains` 整定；参数走 YAML 出厂 + flash 记忆管线。

## 2. 非目标（本期不做）

- TRAP_TRAJ 梯形轨迹、circular_setpoints、gain scheduling、homing/参考系、spinout 检测。
- 位置环独立采样链（不新增 SPI/ADC 读取）。
- CH0/CH1 硬件同步启动（见 §5 相位策略——串行化已足够）。

## 3. ODrive 参考结构（源码取证，firmware master `controller.cpp`）

```text
INPUT_MODE_POS_FILTER（二阶临界阻尼跟踪滤波，控制率同拍执行）：
    delta_pos = input_pos - pos_setpoint
    delta_vel = input_vel - vel_setpoint        ← input_vel 进滤波器
    accel     = kp_f*delta_pos + ki_f*delta_vel
    vel_setpoint += dt*accel
    pos_setpoint += dt*vel_setpoint
  其中 update_filter_gains()：ki_f = 2*bw，kp_f = 0.25*ki_f² = bw²（临界阻尼），
  bw 钳位 ≤ 0.25*fs（ODrive 同源约束）。

位置 P：
    vel_des = vel_setpoint + pos_gain*(pos_setpoint - pos_est)
    vel_des = clamp(vel_des, ±vel_limit)        ← enable_vel_limit

速度环：
    torque = vel_gain*(vel_des - vel_est) + integrator + torque_ff
```

本工程映射：速度环 PI = 现有 `algo_motion_speed`（条件积分抗饱和）+ 摩擦前馈
`algo_friction_torque(vel_cmd)`（本项目替代 ODrive 通用 current_feedforward 的模型化实现）+
`input_torque`（命令力矩前馈），输出按 `reducer·Kt·motion_iq_max_a` 钳位（现状）。

## 4. 架构（用户定案）

- **位置环挂 CH0 独立 5kHz 拍**（`app_motion_timer_bind_position` 新增，镜像 velocity 绑定），
  入口 `app_motion_position_step()`（`ATTR_RAMFUNC`，有界、无 RTT、无 flash）。
- **角度/速度反馈取自速度环 10kHz 估计器**（`app_motion_step` 内部状态），CH0 不做任何 SPI 读取。
- 时序：GPTMR1 CH0/CH1 同实例共享 IRQ、驱动按 CH0→CH1 顺序分发 ⇒ 两回调**天然串行**，
  无并发；CH0 写 `vel_cmd`（单 float）由 CH1 消费，最坏陈旧一个位置拍（200µs），
  相对 pos_gain 带宽（~10-100 1/s）可忽略。foundation spec 的"锁存策略"就此定案：
  **共享 IRQ 串行 + 单 float 交接 + 读取最新估计器状态**，不追求严格 CH0 先于每第二个 CH1。
- **单圈绝对值模式（2026-10-17 变更）**：命令输入 0~360°（内部 rad [0,2π]），控制反馈 =
  单圈绝对轴角 `shaft_rad`，误差/滤波均取模最短路径（±180°，ODrive circular 语义）。
  多圈累积已移除（该模式下无用）；`motion zero` 重置机械零点即坐标系重置。

## 5. 数据流（5kHz CH0 拍）

```text
app_motion_position_step()：
  active && mode==POSITION && 反馈新鲜？
    否 → 返回（输出撤销/保持仍由 10kHz 拍全权处理）
  pos/vel 滤波（§3 POS_FILTER，dt=200µs，bw 消费参数）
  vel_cmd = vel_setpoint + pos_gain*(pos_setpoint - position_rad)
  vel_cmd = clamp(vel_cmd, ±motion_speed_limit)（限速参数 >0 时）
  写 s_vel_cmd_rad_s

app_motion_step()（10kHz，POSITION 分支与 SPEED 分支同构）：
  torque = algo_motion_speed(pi, s_vel_cmd_rad_s, speed, kp, ki, dt, limit)
         + algo_friction_torque(vel_cmd)（辨识挂起时为 0）
         + s_input_torque_ff
  按 torque_limit 钳位 → FOC 提交（现状）
```

## 6. 接口

### 6.1 API（`app_motion.h`）

```c
typedef enum { APP_MOTION_MODE_TORQUE = 0, APP_MOTION_MODE_SPEED, APP_MOTION_MODE_POSITION }
    app_motion_mode_t;
int app_motion_position(float position_rad, float vel_ff_rad_s, float torque_ff_nm);
int app_motion_position_gains(float pos_gain, float filter_bandwidth_rad_s);
```

- `app_motion_position`：单圈绝对值 rad [0,2π]（越界 TARGET_LIMIT）；ODrive
  `input_pos`/`input_vel`/`input_torque` 三设定一次性发布
  （终端节拍内自洽）。校验与 `motion_command` 同链，追加：
  `|vel_ff| ≤ motion_speed_limit`（限速 >0 时）、`|torque_ff| ≤ reducer·Kt·iq_max`；
  `pos_gain ≤ 0` → `GAIN_NOT_SET`。活动中重复发布 = 目标更新（与 speed 一致）；
  活动中切换模式 → `MODE_LOCKED`（提示语更新为 torque/speed/position）。
- `app_motion_position_gains`：仅 idle 可改（与 `app_motion_gains` 同纪律）；
  `pos_gain ∈ (0,100]`，`filter_bandwidth ∈ (0,100]`（0=未配置）。
  写透 `app_software_params_mutable()`（与摩擦增益同路径）。

### 6.2 终端

- `motion pos <deg 0~360> [vel_ff_rad_s] [torque_ff_nm]`（单圈绝对值；360°=0°）
- `motion pos_gains <pos_gain> [input_filter_bandwidth]`（idle）
- `motion show` 增打：mode、position_rad、pos_setpoint_rad、vel_cmd_rad_s。

### 6.3 参数（YAML `control.position_loop`，schema 同步）

| 键 | 出厂值 | 单位 | 说明 |
|---|---|---|---|
| `pos_gain` | 20.0 | 1/s | 位置 P（ODrive 默认值） |
| `input_filter_bandwidth` | 10.0 | rad/s | 二阶滤波带宽（ODrive 默认值；建议≈命令发送频率） |

## 7. 算法层（`algo_motion.h/.c`，纯函数、无硬件依赖）

```c
typedef struct { float pos_setpoint_rad, vel_setpoint_rad_s; } algo_motion_pos_filter_t;
/** @brief ODrive POS_FILTER 单步：bw 钳位≤0.25/dt；输出更新 f 内 pos/vel 设定 */
void algo_motion_pos_filter_step(algo_motion_pos_filter_t* f, float input_pos, float input_vel,
                                 float bandwidth_rad_s, float dt);
/** @brief 位置P+速度前馈：vel = vel_ff + pos_gain*(pos_sp - pos_est) */
float algo_motion_position(float pos_setpoint, float pos_est, float pos_gain, float vel_ff);
```

## 8. 测试与验收

- 算法：阶跃无超调（临界阻尼）、收敛 ~5/bw、稳态跟踪、bw 钳位、P+FF 数学。
- 控制：命令分类（MODE_LOCKED/GAIN_NOT_SET/TARGET_LIMIT/…）、2:1 拍交织收敛（mock 植物）、
  多圈 unwrap 连续、zero/apply 坐标系重置、vel_ff/torque_ff 作用、限速钳位。
- 全量回归 + `make artifacts` 无警告 + 热路径反汇编无 Flash 调用。

## 9. 风险与限制

- 位置环整定未上板：pos_gain/bw 出厂值为 ODrive 默认，需现场整定。
- 滤波器 `input_vel` 语义含在 vel_setpoint 中（ODrive 同构）——`vel_ff` 不重复加到 vel_cmd。
- `speed_max_rad_s`（FOC 层限速兜底）当前为 0（测试期关闭）；位置环 `vel_cmd` 钳位用
  `motion_speed_limit_rad_s`（轴端口径），语义与速度命令一致。
- 位置误差取模最短路径：跨越 ±180° 的目标一律走短角；需要长路径行程时改用速度模式。
- `motion show` 的 `position: sp=` 行显示取模单圈设定（滤波器内部为连续量）。
