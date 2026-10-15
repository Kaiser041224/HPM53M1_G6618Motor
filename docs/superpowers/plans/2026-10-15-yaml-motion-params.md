# YAML 出厂参数 + 在线改 + flash 持久化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 力矩/速度环参数以 YAML 为出厂默认值，在线 `param set`/`motion gains` 可改且掉电保存；YAML 控制值决定本次烧录是否清除 flash 记忆值。

**Architecture:** 复用现有参数管线（config/*.yaml → gen_params.py → params_generated 常量 + meta 表 + app_*_params 单例）。新增通用"带代次记录"读写（app_param_record），motor/software 两域各存一条 flash 记录（MPRM/SPRM），boot 时代次或长度不符则忽略记录、用出厂值；`param save`（OFF 联锁）覆盖写回。motion 在 init 期消费参数并缓存。

**Tech Stack:** C17 / HPM53M1 / Python3 参数生成器 / 宿主 mock 回归。

**Spec:** 本文件即设计记录；用户需求（2026-10-15）：YAML=出厂设置、在线改+掉电保存并存、YAML 控制值决定烧录清除。

## Global Constraints

- 不改 flash 扇区布局（复用末尾 8KB 参数区，app_param 记录制）；不动 MOTN/ENCD/FENC/FCUR 现有记录。
- flash 写仅由停机窗口命令触发（`param save` 前置 FOC OFF 联锁）；boot 不写 flash。
- hardware 域**不持久化**（板级硬事实 + 安全阈值，防在线改写覆盖；决策）。
- 烧录清除控制（决策）：`software.params_generation`（u32）。flash 记录头带代次；
  boot 代次/长度不符 → 忽略记录用出厂值（功能上即"清除并替换"），下次 `param save` 覆盖写回。
  bump 该值 = 本次烧录回收出厂；不采用布尔开关（布尔置 1 会每 boot 清除，破坏掉电保存）。
- motion 参数消费在 init 期缓存（apply=reboot）；`motion gains` 保留 OFF 联锁在线改，
  写透 `app_software_params_mutable()`（`param get` 可见），持久化走 `param save`。
- 记录槽约束：`8 + sizeof(domain)` ≤ `APP_PARAM_DATA_MAX(112)`，`_Static_assert` 卡死。
- 命名无缩写；热路径仍无 Flash libm 调用。

## Task 1: 参数 schema 扩展（YAML + 生成器 + 结构体头文件）

**Files:** `config/motor.yaml`、`config/software.yaml`、`scripts/gen_params.py`、
`App/Control/Inc/app_motor_params.h`、`App/Platform/Inc/app_software_params.h`

新增字段（三处同步：YAML 值 / SCHEMA 行 / 结构体字段）：
- `motor.reducer_ratio` f32=50.0（转矩换算 τ=Kt·iq·reducer；替换代码硬编码 50）
- `software.control.speed_loop.friction_feedforward_nm` f32=0.8（空载起点值）
- `software.control.speed_loop.velocity_lpf_coeff` f32=0.03（测速一阶滤波）
- `software.control.limits.motion_iq_max_a` f32=1.0（motion 命令电流限）
- `software.control.limits.motion_speed_limit_rad_s` f32=0.0（0=不限）
- `software.params_generation` u32=1（烧录清除控制）
- 既有 `speed_loop.kp/ki` 填空载实测起点值 2.0/50.0（注释注明来源）
- APPLY_REBOOT 增以上消费项（除 params_generation 外均 reboot 语义）

- [x] Step 1: `python3 scripts/test_gen_params.py` 先红（新键未入 SCHEMA 会 unknown key）→ 改 SCHEMA/头文件/YAML → 绿。
- [x] Step 2: `python3 scripts/gen_params.py --config-dir config --out-dir build/generated --check` 通过。

## Task 2: 带代次记录读写 + 持久化（新宿主测试套件）

**Files:** Create `App/Platform/Inc/app_param_record.h`、`App/Platform/Src/app_param_record.c`、
`scripts/tests/params/{run.sh,mock_flash.c,test_main.c}`；Modify `App/Platform/Inc/app_param.h`（键）、
`App/Control/Src/app_motor_params.c`、`App/Platform/Src/app_software_params.c`、`App/Platform/Src/app_hardware_params.c`（注释）

- 接口：`int app_param_record_load(uint32_t key, void* payload, size_t payload_size, uint32_t generation);`
  `int app_param_record_store(uint32_t key, const void* payload, size_t payload_size, uint32_t generation);`
  记录=头{generation, payload_size}+payload；load 校验代次+长度，不符返回-1（调用方保持出厂值）。
- 键：`APP_PARAM_KEY_MOTOR_PARAMS "MPRM"`、`APP_PARAM_KEY_SOFTWARE_PARAMS "SPRM"`。
- `app_motor_params_load()/app_software_params_load()` 出厂值+flash 叠加；新增 `app_*_params_store()`。
- [x] Step 1: params 套件失败测试：store→load 回读一致；代次不符→-1 且内存不变；长度不符→-1。
- [x] Step 2: 红→实现→绿；`_Static_assert` 记录≤112B。
- [x] Step 3: app_*_params store/load 往返 + 代次 bump 后回落出厂值。

## Task 3: motion 消费参数 + 命令限幅

**Files:** `App/Control/Src/app_motion.c`、`scripts/tests/control/{mock_platform.c,test_motion.c}`

- init 缓存：speed_loop.{kp,ki,friction_feedforward_nm}、velocity_lpf_coeff、
  limits.{motion_iq_max_a,motion_speed_limit_rad_s}、motor.reducer_ratio。
- 转矩换算/命令限幅/PI 限幅用 `reducer_ratio·kt·motion_iq_max_a`（原 50·kt）；
  速度命令限幅 `motion_speed_limit_rad_s>0` 时生效（默认 0=不限，保持用户决定）。
- 测速滤波系数取参数；`motion gains` 写透 software params 单例。
- [x] Step 1: 失败测试：mock 参数置 kp/ki/ff/limit/reducer 后 init → `motion speed` 直接可用、
  限幅/换算按参数、gains 写透 `param get` 可见、lpf 系数影响测速。注意 mock_params_reset 需给 reducer_ratio=50。
- [x] Step 2: 红→实现→绿（control 全套）。

## Task 4: `param save` 命令 + 文档 + 全量验证

**Files:** `App/Comm/terminal/Src/app_terminal_cmd_param.c`、docs、全部测试与构建

- `param save`（FOC OFF 联锁）→ 存 motor+software 两条记录；报告逐域结果。
- [x] Step 1: 文档（本计划勾选 + spec 记录）；`python3 scripts/test_gen_params.py` + 四套回归 + UBSan + `make artifacts` 无警告。
- [x] Step 2: 反汇编复核热路径无新增 Flash 调用；暂存整理 + 提交信息（不 commit）。

## Review Focus

- 记录长度/代次错配不得让非法数据进控制：load 失败必须整条忽略（不能部分叠加）。
- `param save` 在 FOC 非 OFF 时必须拒绝（flash 纪律）。
- mock_params_reset 遗漏新字段会让旧测试除零/误判：reducer_ratio=0 的除法风险。
- speed_loop 出厂值非 0 后，"增益未配置"路径只在显式清零时出现：测试需覆盖两态。
- hardware 域 `param set` 改动不持久化：文档与 `param save` 回显需明示。

## Self-Review

覆盖：YAML 出厂值(T1)、在线改+掉电保存(T2/T3)、烧录清除控制(T1/T2)、命令入口(T4)；
类型一致（reducer_ratio/motion_*命名全计划统一）；Review Focus 五项均绑定任务测试。
