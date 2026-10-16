# 摩擦前馈（路线2）：模型 + 自整定 + 手动接口 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans。步骤用 `- [ ]` 追踪。

**Goal:** 实施 M2 摩擦模型（平滑库仑+粘性）+ R-A 自动辨识 + R-B 手动接口，参考 ODrive
（显式前馈项 + 机上学习流程 + 持久化配置）。

**Architecture:** `algo_friction`（纯算法：模型求值/最小二乘拟合）→ `app_friction_identify`
（隔离辨识状态机，照 app_motor_identify 风格：start/run_once/abort + 结果快照）→
motion 消费模型（速度模式 `τ += Tc·tanh(ω_ref/ω_s) + Kv·ω_ref`）→ 终端
`motion gains kp ki [coulomb] [viscous]`（手动）与 `motion friction show|cal|apply|save`（自动）。
持久化走参数管线（schema 更名+扩展，代次语义不变）。

**Spec:** `docs/superpowers/specs/2026-10-15-friction-feedforward-design.md`（用户已选路线2）。

## Global Constraints

- 模型仅作用于速度模式；力矩模式不加前馈。按目标速度符号求值，输出仍按 reducer·Kt·iq_max 钳位。
- schema：`friction_feedforward_nm` **更名** `friction_coulomb_nm`（同位置同类型，flash 记录
  布局兼容），新增 `friction_viscous_nm_per_rad_s`（≥0）与 `friction_smoothing_rad_s`（>0）。
  两者随新字段使记录长度变化 → 旧记录长度不符自动回落出厂（代次语义兜底，无需 bump）。
- 手动接口：`motion gains kp ki [coulomb_nm] [viscous]`（OFF 联锁，写透参数单例）；
  平滑系数为模型常量，仅 YAML。
- 自整定：`motion friction cal`（FOC READY/RUN + motion 已标定 + 未活动；速度闭环下
  正/反 ±{0.05,0.1,0.2,0.4,0.8,1.0} rad/s 共12点，每点 settle 1.2s + 采样 0.5s 平均
  (speed, torque_nm)）；辨识数据 = 稳态 motion 总力矩请求（=摩擦，与 FF/PI 分成无关）。
- 结果校验：拟合 RMS 残差 ≤ 20% 均值；正反库仑对称性 ≤ 20%；超限 = 失败，
  **不 apply/save**（电角校准教训）。单点超时 3s → 中止失败。
- `apply`（FOC OFF）：结果写参数单例（经 app_motion_gains 写透）；`save`（FOC OFF）：
  `app_software_params_store()`。cal 结束自动 `motion off`。
- 外部命令互斥：identify busy 时终端 `motion` 除 `off` 外拒绝（内部不加锁，避免自锁）。
- 命名无缩写；热路径无 Flash libm（tanh 需等价有界近似或查表——用 `x/(1+|x|)` 型有理
  近似或分段线性，禁 libm tanh 闪调）。

## Task 1: algo_friction 纯算法

**Files:** Create `App/Algorithm/Inc/algo_friction.h`、`App/Algorithm/Src/algo_friction.c`；
Test: `scripts/tests/control/test_friction_math.c`（注册 test_main）

- `typedef struct { float coulomb_nm, viscous_nm_per_rad_s, smoothing_rad_s; } algo_friction_params_t;`
- `float algo_friction_torque(const algo_friction_params_t* p, float speed_rad_s);`
  （平滑符号 `sign(x)≈x/(smoothing+|x|)` 有理近似？不——用 tanh 近似需保符号与单调；
  采用分段：|ω|<ω_s 时线性 ω/ω_s，外侧 sign——即 `ω/(ω_s+|ω|)·(ω_s+|ω|)/...` 待定，
  以测试钉住：奇函数、|ω|>>ω_s 趋 Tc、|ω|→0 趋 0、连续无跳变）
- `int algo_friction_fit(algo_friction_params_t* out, float* residual_rms_out,
  const float* speeds, const float* torques, unsigned count);`（≥4点、双符号、有限值；
  最小二乘解 [Tc,Kv]：τ=Tc·sign(ω)+Kv·ω；平滑系数沿用入参/默认）
- [x] 测试先行：完美数据恢复参数、噪声数据、单符号数据、点数不足/NaN/全零速度拒绝、
  模型求值边界（零速=0、奇对称、大速度趋 Tc·sign+Kv·ω）。
- [x] 红→实现→绿。

## Task 2: 参数 schema + motion 消费 + 手动接口

**Files:** `config/software.yaml`、`scripts/tests/fixtures/software.yaml`、`scripts/gen_params.py`、
`App/Platform/Inc/app_software_params.h`、`App/Control/Inc/app_motion.h`、`App/Control/Src/app_motion.c`、
`App/Comm/terminal/Src/app_terminal_cmd_foc.c`、`scripts/tests/control/{mock_platform.c,test_motion.c}`

- schema/结构体更名+扩展（见 Global）；APPLY_REBOOT 同步。
- motion：`app_motion_gains(kp, ki, coulomb_nm, viscous_nm_per_rad_s)`；速度模式前馈改模型求值。
- [x] 失败测试：gains 四参写透；τ_ff = coulomb·smooth_sign + viscous·ω 行为钉住
  （零速/大速度/换向）；YAML 消费（mock 参数）。
- [x] 红→实现→绿（control 全套 + gen_params 20/20）。

## Task 3: app_friction_identify 辨识状态机

**Files:** Create `App/Control/Inc/app_friction_identify.h`、`App/Control/Src/app_friction_identify.c`；
Test: `scripts/tests/control/test_friction_identify.c`；CMakeLists 注册

- API：`init/start/run_once(1kHz)/abort/get_snapshot`；状态
  IDLE→SETTLING→SAMPLING(逐点)→FITTING→DONE/FAILED；结果快照（参数+残差+对称性+失败原因）。
- 采样：`app_motion_get_snapshot()` 的 torque_nm/speed_rad_s 窗口平均；单点超时/运动失效/
  iq 超限 → FAILED（不落盘）。
- [x] 失败测试：进度推进（mock 转子随 iq 走的简易被控对象）、超时失败、
  运动撤销失败、DONE 才允许 apply。
- [x] 红→实现→绿。

## Task 4: 终端 + 文档 + 全量验证

**Files:** `app_terminal_cmd_foc.c`（motion friction show|cal|apply|save + busy 互斥）、docs、构建

- [x] `motion friction` 四子命令与 busy 互斥（off 除外）；show 显示模型/结果。
- [x] 文档（spec 追加实施记录）+ gen_params 20/20 + 四套回归 + params + UBSan + `make artifacts`
  无警告 + 热路径反汇编无 Flash 调用 + 暂存/提交信息（不 commit）。

## Review Focus

- 拟合数据含 FF 旧值仍正确（总力矩=摩擦，与分成无关）——测试用带 FF 的运动数据验证。
- smooth_sign 近似的零速连续性与符号正确性（负速度不得给正前馈）。
- 辨识中收到 motion 坏拍撤销 → 必须失败且不写参数。
- apply/save 的 OFF 联锁与"失败不落盘"。
- 旧 flash 记录在字段更名/扩展后的兼容行为（长度不符→出厂值，不得部分叠加）。
