# DM 兼容 CAN 控制协议实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 MCAN3 上以 DM 手册的载荷格式实现四类控制帧、统一反馈帧、上电读取 DIP 节点号并移除 CANID 采样槽。

**Architecture:** `app_can_protocol` 只做纯编解码；`app_can_service` 在 1kHz 任务里做解析、动作规划和回帧；运动层新增 per-command 限速/限流与角度回报开关；节点号在 `app_init()` 中从 ADC1 读一次后固定。

**Tech Stack:** C17、HPM SDK 1.12.1、FreeRTOS 非抢占、主机 cc 测试（-O1 / -O2 -ffast-math 两档）。

**Spec:** `docs/superpowers/specs/2026-11-02-dm-can-protocol-design.md`

## Global Constraints

- 所有 ID 固定为 `0x100 | (type << 4) | node`，`type` 1..8，`node` 0..15；本机节点号来自 §2.3.1 的 8 档表。
- 节点号阈值（raw，`>=` 进档，按**电压低→高**排序）：`{6120, 14014, 19653, 28120, 34261, 36284, 38487}`；ID = 拨码二进制 `S1<<2 | S2<<1 | S3`（000→0、001→1、…、111→7）。电压档位→ID 置换表：`{0,4,1,5,2,6,3,7}`。
- 线缆定标：`P_MAX=12.5`、`V_MAX=30.0`、`T_MAX=10.0`、`KP_MAX=500.0`、`KD_MAX=5.0`。
- `i_des` 分母 = `motor.i_peak_10s_a`（当前 24.3 A）。
- 反馈 ID = `0x180 | node`；位置为输出轴单圈 `[-π, π)`，转矩用 `kt × measured_iq × reducer × output_direction`。
- 温度无效上报 `0xFF`；ERR 优先级 `A > 8 > 9 > B > C > D > F`。
- `app_software_params_t` 布局不变：`node_id_default` 原地改名 `can_id`；`rx_control_id`/`tx_report_id`/`control.mit.can_format` 保留为 reserved。
- ISR 零协议逻辑；每 1kHz 拍最多处理 8 帧、回 8 帧。
- 新增热路径函数必须 `FOC_ATTR_RAMFUNC` 且通过 `check_ilm_calls.sh`；ILM 内不得调用 libm。
- ADC1 5→4 槽改动后必须重跑 §9.5.2 vbus 配对 A/B 与 `cal rl`（台架项，不阻塞代码合并）。

## Review Focus

1. **节点号误判**：阈值边界、ADC 抖动、000 档偏置（实测 508）——必须落在正确的 ID，不能落到相邻档。
2. **浮点越界/非有限**：位置/速度帧的 float32 为 NaN/Inf 或远超 `P_MAX/V_MAX`——必须丢帧，不得进入 motion。
3. **模式切换**：运动进行中收到不同模式帧——必须先释放输出并丢帧，下一拍才接受新模式，不能静默拒绝。
4. **反馈有效性**：未标定、`feedback_valid=0`、速度/力矩模式未开角度回报——反馈位置不能是冻结值而不报 ERR=F。
5. **ADC 槽释放失败**：ADC1 未出帧或重配后帧计数不推进——必须保留 5 槽并回退 YAML 节点号，不能把 CANID 读成陈旧值。

---

## Task 1: DM 协议纯函数与反馈编码

**Files:**
- Create (overwrite): `App/Comm/can/Inc/app_can_protocol.h`
- Create (overwrite): `App/Comm/can/Src/app_can_protocol.c`
- Test: `scripts/tests/can/test_can_codec.c`、`scripts/tests/can/test_main.c`

**Interfaces:**
- Consumes: 无（纯 C，无工程依赖）。
- Produces:
  - `typedef struct { float p_max_rad, v_max_rad_s, t_max_nm, kp_max, kd_max; } app_can_dm_scale_t;`
  - `const app_can_dm_scale_t* app_can_dm_default_scale(void);`
  - `bool app_can_dm_parse_id(uint32_t id, uint8_t* type, uint8_t* node);`
  - `int app_can_dm_decode(uint32_t id, const uint8_t* data, uint8_t len, uint8_t node, const app_can_dm_scale_t* scale, app_can_dm_frame_t* out);`（0=解码成功；1=非本机/未知类型；-1=长度或参数非法）
  - `uint8_t app_can_dm_encode_feedback(uint8_t* out, uint8_t node, uint8_t err, float pos_rad, float vel_rad_s, float torque_nm, uint8_t t_mos_c, uint8_t t_rotor_c, const app_can_dm_scale_t* scale);`
  - `uint8_t app_can_dm_err_from_fault(uint32_t latched, bool feedback_valid, bool enabled);`
  - `uint32_t app_can_dm_float_to_uint(float x, float min, float max, uint32_t bits);`
  - `float app_can_dm_uint_to_float(uint32_t x, float min, float max, uint32_t bits);`
  - `app_can_dm_frame_kind_t`：`NONE/MIT/POS_VEL/VEL/FORCE_POS/ENABLE/DISABLE/CLEAR`。
  - `typedef struct { app_can_dm_frame_kind_t kind; float p_des_rad, v_des_rad_s, kp, kd, t_ff_nm; float pos_rad, vel_limit_rad_s, iq_limit_a; } app_can_dm_frame_t;`

- [ ] **Step 1: 写失败测试**（`scripts/tests/can/test_can_codec.c`）

```c
/* ID：0x111 -> type=1 node=1；0x187 -> type=8 node=7；0x11F node=15；
 * 0x1A1/0x211/0x100/0x190 均非法 */
CHECK(app_can_dm_parse_id(0x111U, &t, &n) && t == 1U && n == 1U);
CHECK(!app_can_dm_parse_id(0x100U, &t, &n));
CHECK(!app_can_dm_parse_id(0x1A1U, &t, &n));
/* MIT 位打包：p=0x7FFF, v=kp=kd=t=0x7FF -> 7F FF 7F F7 FF 7F F7 FF */
const uint8_t mit[8] = {0x7F,0xFF,0x7F,0xF7,0xFF,0x7F,0xF7,0xFF};
/* 反馈：node=5 err=1 pos=0 vel=0 t=0 t_mos=40 t_rotor=50
 * -> 15 7F FF 7F F7 FF 28 32 */
/* ERR 优先级：VBUS_OV|OC_FAST_U -> 'A'；VBUS_OV|COMM_TIMEOUT -> '8' */
/* 越界饱和：float_to_uint(1e9) == max；float_to_uint(-1e9) == min */
```

- [ ] **Step 2: 运行并确认失败**

Run: `bash scripts/tests/can/run.sh`
Expected: 编译失败（新接口未定义）。

- [ ] **Step 3: 实现 `app_can_protocol.[ch]`**

按 spec §2.2/§3.1-3.5/§5 实现：位读写用逐字节移位，不用指针强转；`float_to_uint` 截断不四舍五入；`app_can_dm_decode` 先校验 `(id >> 8) == 0x1` 与 `(id & 0x0F) == node`，再按 `type` 分派；`len` 要求见 spec（VEL 接受 4..8）。

- [ ] **Step 4: 运行并确认通过**

Run: `bash scripts/tests/can/run.sh`
Expected: 两档 pass，0 failures。

- [ ] **Step 5: 提交**

```bash
git add App/Comm/can/Inc/app_can_protocol.h App/Comm/can/Src/app_can_protocol.c scripts/tests/can/
git commit -m "feat(can): DM 控制/反馈报文纯编解码"
```

---

## Task 2: 运动层 per-command 限幅与角度回报

**Files:**
- Modify: `App/Algorithm/Inc/algo_ramp.h`、`App/Algorithm/Src/algo_ramp.c`
- Modify: `App/Control/Inc/app_motion.h`、`App/Control/Src/app_motion.c`
- Test: `scripts/tests/control/test_motion.c`、`scripts/tests/control/test_algo_ramp_plan.c`

**Interfaces:**
- Consumes: 现有 `algo_ramp_plan`、`app_motion_position` 路径。
- Produces:
  - `int algo_ramp_set_limits(algo_ramp_t* self, float v_max, float a_max);`（梯形模式；`v_max>0 && a_max>0` 才生效，返回 0/-1）
  - `int app_motion_position_limited(float position_rad, float vel_limit_rad_s, float torque_ff_nm, float iq_limit_a);`
  - `void app_motion_set_angle_report_required(bool required);`

- [ ] **Step 1: 写失败测试**

```c
/* algo_ramp：v_max=2 -> 走 10 rad 的时间 >= 5 s；v_max=10 -> < 2 s */
/* motion_position_limited：vel_limit=1 时 traj_vel_rad_s 不超过 1.0 + 1e-3 */
/* iq_limit=2 A 时 torque_nm <= kt*reducer*2 + 1e-4 */
/* speed 模式下 set_angle_report_required(true) 后 shaft_rad 随 mock pair 变化；
 * false 时保持上一值 */
```

- [ ] **Step 2: 运行并确认失败**

Run: `bash scripts/tests/control/run.sh`
Expected: 新符号未定义。

- [ ] **Step 3: 实现**

`algo_ramp_set_limits` 直接写 `_v_max/_a_max`（`_mode == TRAPEZOIDAL` 才允许）。`app_motion_position_limited` 复用 `motion_command` 位置分支：命令入口前调 `algo_ramp_set_limits(&s_position_traj, min(vel_limit, YAML limit), YAML accel)`；`iq_limit` 写入本次命令的 `s_cmd_iq_limit`（0=不额外限制），在 `app_motion_step` 的转矩输出处与 `s_motion_iq_max_a` 取小。`app_motion_set_angle_report_required` 增加一个与 `s_output_tracking`/`s_cogging_output_required` 并列的 `s_report_tracking`，OR 进 `need_pair` 和 `rotor_only` 判定。

- [ ] **Step 4: 运行并确认通过**

Run: `bash scripts/tests/control/run.sh`
Expected: 两档 pass，0 failures。

- [ ] **Step 5: 提交**

```bash
git add App/Algorithm App/Control scripts/tests/control
git commit -m "feat(motion): per-command 限速/限流与 CAN 角度回报"
```

---

## Task 3: NTC 温度换算

**Files:**
- Modify: `App/Platform/Inc/app_analog_signal.h`、`App/Platform/Src/app_analog_signal.c`
- Test: `scripts/tests/analog/test_analog_signal.c`

**Interfaces:**
- Produces: `bool app_analog_signal_ntc_temp_c(adc_channel_t ch, float* temp_c);`（仅 NTC0/NTC1；R 在 `(100, 1e6)` 且有限才返回 true）

- [ ] **Step 1: 写失败测试**

```c
/* R=10K, R25=10K, B=3950 -> 25.0°C ±0.2 */
/* R=1M（开路替代）-> false；R=0 -> false；NaN -> false */
```

- [ ] **Step 2: 运行并确认失败**

Run: `bash scripts/tests/analog/run.sh`
Expected: 符号未定义。

- [ ] **Step 3: 实现**

`T = 1/(1/298.15 + lnf(R/R25)/B) - 273.15`；先做范围与有限值检查（用位模式，不用 `isfinite`，兼容 `-ffast-math`）。`R25/B` 取 `app_hardware_params_current()->ntc`。

- [ ] **Step 4: 运行并确认通过**

Run: `bash scripts/tests/analog/run.sh`
Expected: 两档 pass。

- [ ] **Step 5: 提交**

```bash
git add App/Platform scripts/tests/analog
git commit -m "feat(analog): NTC 阻值转摄氏温度"
```

---

## Task 4: 节点号读取与 CANID 采样槽释放

**Files:**
- Create: `App/Platform/Inc/app_can_id.h`、`App/Platform/Src/app_can_id.c`
- Modify: `App/Platform/Inc/app_adc.h`、`App/Platform/Src/app_adc.c`
- Modify: `Interface/intf_adc.h`、`Interface/intf_default.c`、`Driver/hpm_impl/drv_adc.c`
- Modify: `App/Logic/app_logic.c`
- Test: `scripts/tests/adc/test_app_adc_layout.c`、`scripts/tests/adc/mock_deps.c`

**Interfaces:**
- Produces:
  - `uint8_t app_can_id_read_once(void);`（返回 0..7；失败返回 `0xFF`）
  - `int app_adc_canid_release(void);`（0=4 槽重配成功；-1=保留 5 槽）
  - `int intf_adc_seq_reconfigure(intf_adc_ch_t ch, const intf_adc_cfg_t* cfg);`
- 阈值表放在 `app_can_id.c`：`static const uint16_t s_canid_thresholds[7] = {6120,14014,19653,28120,34261,36284,38487};`（生成器不支持 YAML 数组，板级事实用 C 表）。

- [ ] **Step 1: 写失败测试**

```c
/* 8 个 raw（508,11733,16295,23011,33230,35292,37277,39697）-> ID {0,4,1,5,2,6,3,7} */
/* raw=6119 -> 0；raw=6120 -> 4；raw=38487 -> 7 */
/* ADC 未出帧 -> app_adc_canid_release() == -1，保留 5 槽 */
/* 出帧后 -> 0；mock 记录 seq_ch_count == 4 且槽序 [VBUS,NTC0,NTC1,VBUS] */
```

- [ ] **Step 2: 运行并确认失败**

Run: `bash scripts/tests/adc/run.sh`
Expected: 符号未定义。

- [ ] **Step 3: 实现**

`app_can_id_read_once`：轮询 `intf_adc_get_seq_frames(1)` 变化（或 2ms 超时）→ `app_adc_slow_process()` → `app_adc_get_raw(ADC_CH_V_CANID,&raw)` → 递增阈值判定；失败回退 `app_software_params_current()->can.can_id`（YAML）并打印 WARN。`app_adc_canid_release`：`intf_adc_seq_reconfigure` 传入 4 槽配置；成功则把 `s_slow_channels` 降为 3 项、`APP_ADC_SLOW_SEQ_COUNT` 改为运行期变量；失败保留 5 槽并返回 -1。驱动侧：`adc16_seq_disable_hw_trigger` → 重写 `SEQ_QUE`/`SEQ_CFG0`（`adc16_set_seq_config`）→ `adc16_seq_enable_hw_trigger`；重配前后清 `SEQ` 完成标志。

`app_analog_signal` 在释放后把 `ADC_CH_V_CANID` 的 `s_valid_mask` 位清掉，`adc dump` 显示 `no data`；`app_debug_adc.c` 的启动打印改为「5 槽启动 / 读 ID 后 4 槽」。`config/hardware.yaml` 的 `canid_dip.levels` 改为 8（文档值；实际阈值表在 `app_can_id.c`）。

`app_logic.c`：在 `app_adc_init()` 之后新增步骤 11b：

```c
uint8_t can_node = app_can_id_read_once();
if (can_node == 0xFFU) can_node = software->can.can_id;
(void)app_adc_canid_release();
app_can_service_init(can_node);
```

把原步骤 5 的 `app_can_service_init()` 删掉；`app_debug_can_init()` 保持原位。

- [ ] **Step 4: 运行并确认通过**

Run: `bash scripts/tests/adc/run.sh && bash scripts/tests/can/run.sh`
Expected: 两档 pass。

- [ ] **Step 5: 提交**

```bash
git add App/Platform App/Logic Interface Driver/hpm_impl scripts/tests/adc
git commit -m "feat(can): 上电读取 DIP 节点号并释放 CANID 采样槽"
```

---

## Task 5: CAN 服务重写

**Files:**
- Create (overwrite): `App/Comm/can/Inc/app_can_service.h`
- Create (overwrite): `App/Comm/can/Src/app_can_service.c`
- Test: `scripts/tests/can/test_can_service.c`、`scripts/tests/can/mock_service_deps.c`、`scripts/tests/can/run.sh`

**Interfaces:**
- Consumes: Task 1 的编解码；`app_motion_*`、`app_foc_*`、`app_fault_*`、`app_can_*`。
- Produces:
  - `void app_can_service_init(uint8_t node_id);`
  - `void app_can_service_step(void);`
  - 保留 `app_can_service_suspend_watchdog` / `feed_watchdog` / `get_watchdog` / `flush_rx` / `get_node_id` / `get_dropped` / `get_stats`。
  - 纯规划函数（供主机测试）：
    ```c
    typedef enum { APP_CAN_DM_ACT_DROP = 0, APP_CAN_DM_ACT_ENABLE, APP_CAN_DM_ACT_DISABLE,
                   APP_CAN_DM_ACT_CLEAR, APP_CAN_DM_ACT_MIT, APP_CAN_DM_ACT_POSITION,
                   APP_CAN_DM_ACT_VELOCITY, APP_CAN_DM_ACT_FORCE_POS,
                   APP_CAN_DM_ACT_RELEASE_OUTPUT } app_can_dm_action_t;
    int app_can_dm_plan(const app_can_dm_frame_t* frame, uint8_t active_mode, bool active,
                        uint8_t foc_state, bool mit_enabled, app_can_dm_action_t* out);
    ```

- [ ] **Step 1: 写失败测试**

```c
/* 0x151 enable -> ACTION_ENABLE；0x161 disable -> ACTION_DISABLE；
 * 0x171 magic 0xFE（OFF）-> ACTION_CLEAR；magic 错 -> 丢弃 */
/* active_mode=POSITION + MIT 帧 -> ACTION_RELEASE_OUTPUT（只一次） */
/* FOC OFF + MIT 帧 -> ACTION_DROP */
/* 反馈：node=3 err=1 pos=0 vel=0 t=0 -> ID 0x183，D0=0x13 */
/* 反馈无效（feedback_valid=false）-> ERR=F，pos/vel/torque 为 0 */
/* 每拍最多 8 条回帧 */
```

- [ ] **Step 2: 运行并确认失败**

Run: `bash scripts/tests/can/run.sh`
Expected: 服务符号未定义。

- [ ] **Step 3: 实现**

`step()`：`app_can_poll()` → 解析队列（≤8）→ `app_can_dm_plan()` → 执行（`app_foc_enable` / `app_motion_stop` / `app_fault_clear` / `app_motion_mit` / `app_motion_position_limited` / `app_motion_speed`）→ 每条有效帧回一帧反馈。MIT 路径用 `snapshot.shaft_continuous_rad × output_direction` 与单圈 `wrap_pm_pi` 计算最近圈目标；`kp/kd` 钳到 `control.mit.kp_max/kd_max`；`i_des` 乘 `motor.i_peak_10s_a`；`v_des` 取绝对值并钳 `[0.01, V_MAX]`。反馈位置/速度/转矩按 spec §5.1；温度用 Task 3；ERR 用 Task 1 的映射。看门狗在 RX ISR 钩子里按新 ID 解析喂狗。

- [ ] **Step 4: 运行并确认通过**

Run: `bash scripts/tests/can/run.sh`
Expected: 两档 pass。

- [ ] **Step 5: 提交**

```bash
git add App/Comm/can scripts/tests/can
git commit -m "feat(can): DM 运行时控制服务（使能/失能/四模式/反馈）"
```

---

## Task 6: 配置迁移与清理

**Files:**
- Modify: `config/software.yaml`、`scripts/tests/fixtures/software.yaml`
- Modify: `App/Platform/Inc/app_software_params.h`
- Modify: `scripts/gen_params.py`（如需改字段名）
- Modify: `App/Debug/Src/app_debug_can.c`（如仍引用旧字段）
- Test: `scripts/tests/params/`、`python3 scripts/test_gen_params.py`

- [ ] **Step 1: 改配置**

```yaml
can:
  baudrate: 1000000
  can_id: 1            # YAML 兜底（DIP 解码失败时用）
  rx_control_id: 0     # reserved，不再使用
  tx_report_id: 0      # reserved，不再使用
```

`app_software_params.h` 的 `node_id_default` 原地改名 `can_id`（u8，偏移不变）；`control.mit.can_format` 保留字段但不再被读取。

- [ ] **Step 2: 跑生成器与参数测试**

Run: `python3 scripts/test_gen_params.py && bash scripts/tests/params/run.sh`
Expected: 全 PASS；SPRM `sizeof` 不变。

- [ ] **Step 3: 更新 AGENTS.md**

CAN 章节补：ID 方案 `0x1|type|node`、8 档 DIP 表位置、反馈格式、模式切换约束、`can_id` 仅兜底。

- [ ] **Step 4: 提交**

```bash
git add config App/Platform scripts/tests/fixtures scripts/gen_params.py AGENTS.md
git commit -m "refactor(params): can_id 取代 node_id_default（布局不变）"
```

---

## Task 7: 集成验证

- [ ] **Step 1: 全量构建与测试**

Run: `make build && SKIP_LAYOUT=1 bash scripts/tests/all.sh`
Expected: `BUILD SUCCESS`、0 warning、全量 PASS。

- [ ] **Step 2: 布局守卫（需先 artifacts）**

Run: `make artifacts && bash scripts/tests/layout/check_layout.sh && bash scripts/tests/layout/check_ilm_calls.sh`
Expected: ILM 54/54、跨段调用白名单内；新增热路径符号在 `.fast`。

- [ ] **Step 3: 台架验收（spec §8.2）**

1. 8 档 DIP 逐档上电，核对节点号与反馈 ID。
2. 使能 → 速度 → 位置 → 力位混控 → 失能。
3. 拔 CAN，确认 `COMM_TIMEOUT` 动作（需 `actions_enable=true`）。
4. `fault inject` 验证 ERR。
5. 1kHz 持续收发 10 分钟。
6. ADC1 5→4 槽后重跑 §9.5.2 A/B 与 `cal rl`。

Expected: 全部通过；任何一项失败按 spec §11 风险表定位。
