# FOC 算法设计：Algorithm/FOC 纯数学层 + Control 编排（V1：电流环 + 电角度辨识）

- 日期：2026-09-21
- 状态：设计已确认（Kaiser），待实施
- 决策人：Kaiser（2026-09-21）
- 关联文档：
  - `2026-09-19-adc-sampling-design.md`（采样链 / 触发时序 / 采样窗口 d_max）
  - `2026-09-18-m1-board-bringup-design.md`（PWM1 三相映射 / 逆变桥 / 引脚）
  - `2026-09-18-encoder-sensing-design.md`（编码器角度链 / 双 KTH7823）
  - `2026-09-20-param-pipeline-design.md`（三域参数管线 / `control.*` 预留）
  - `2026-09-19-fault-protection-design.md`（故障状态 / 联锁）
  - `2026-09-21-usb-terminal-design.md`（Terminal 命令 / job 框架 / monitor）

## 0. 决策记录（Kaiser，2026-09-21）

| 决策项 | 结论 |
| :--- | :--- |
| 电流环频率与位置 | **25kHz，随开关周期同步**（PWM 谷底采样 → 电流环）。V1 在主循环 25kHz 节拍执行；后续可迁 ADC0 ISR（§8.3） |
| 外环频率 | 未来提高外环控制频率；架构上外环按**电流环 tick 分频**调用（分频系数可配，V1 不实现外环） |
| V1 范围 | **仅电流环（转矩模式）**；速度环/位置环后续阶段 |
| 无感 | 暂不实现；`foc_observer` 仅占位（接口预留，不建文件） |
| V1 辨识范围 | **电角度辨识（零点 / 方向 / 极对数校验）为必做**；R/L/Ke 辨识后置，V1 先用手册参考值 |
| 调制方式 | **min-max 零序注入**（等价 SVM，免扇区判断，线性区与 SVPWM 一致） |
| 转矩口径 | 统一为**峰值电流口径**：`T = kt_nm_per_a · i_q`（手册 `kt = 0.117 N·m/A`，与手册转矩表自洽）；dq 矢量幅值 = 相电流峰值。V1 不暴露 N·m 接口（直接给电流 [A]） |

---

## 1. 背景与目标

### 1.1 背景

平台侧 FOC 前置条件已具备：

| 能力 | 现状 | 来源 |
| :--- | :--- | :--- |
| 三相电流采样 | ADC0 PMT @25kHz，`[I_W副本, I_U, I_V, I_W]`，谷底 +500ns 触发，单通道 ≈775ns | ADC spec §4 |
| 电流换算 | `app_analog_signal`（无滤波，仅标定），±100A / 16bit | app_analog_signal.h |
| 逆变桥 | PWM1 三相 25kHz 中心对齐 + 死区 + `force_low` / `emergency_stop` | app_3phase_inverter.h |
| 转子角度 | KTH7823 16bit 绝对，SPI3，转子 1:1，阻塞读 ≈5µs | app_encoder.h |
| 主循环节拍 | 25kHz（`cpu_freq / inverter.pwm_freq_hz`），注释已注明"FOC 闭环同频" | app_logic.c |
| 保护 | `app_fault` 10 故障源（v1 纯判断）+ 状态查询 API | app_fault.h |
| 参数 | `control.current_loop.kp/ki`、`control.limits.*` 已生成未消费 | app_software_params.h |
| 算法库 | `algo_pid / filter / pll / rms / ramp / hyst / ffd` | App/Algorithm |

缺口：FOC 数学层、Control 编排、电角度辨识、平台少量增量接口（§6）。

电机（G66-18 KV70，手册值，V1 直接使用）：

| 参数 | 值 | 说明 |
| :--- | :--- | :--- |
| 极对数 `p` | 10 | 24N20P |
| 相电阻 `Rs` | 0.158 Ω | 线值 ÷2 |
| 相电感 `Ls` | 118.5 µH | 线值 ÷2 |
| 反电动势系数 `Ke` | 0.125 V·s/rad | KV70；与 `Kt` 手册值差异待 V2 辨识校核 |
| 额定 / 峰值电流 | 7.0 A RMS / 24.3 A（10s） | 峰值口径 = 9.9 A / 24.3 A |
| 最高转速 | 3300 rpm → 电频率 550 Hz | 电流环 25kHz → 每电周期 45 拍 |

### 1.2 V1 目标

1. `App/Algorithm/FOC/` 纯数学层建立（电机控制大类 V1 模块 + 辨识大类 V1 模块）；
2. **电流环闭环**（转矩模式）：编码器角度 → Clarke/Park → d/q PI → 圆形限幅 → 反 Park → 零序注入调制 → 三相占空比，25kHz；
3. **电角度辨识**：`cal encoder` 一键辨识电角度零点 / 方向 / 极对数校验，结果写 RAM；
4. Terminal 可观测可操作：`foc` 状态/诊断、`motor iq <A>` 给定、`cal encoder` 辨识；
5. 与故障、编码器、逆变桥的联锁完整（安全停止路径明确）。

### 1.3 非目标（V1 不做）

- 速度环 / 位置环（外环分频点预留）；
- 无感观测器 / HFI（接口占位）；
- R / L / Ke 辨识与电流环增益自整定（V2；V1 用手册值 + 手动 kp/ki）；
- 弱磁 / 过调制 / MTPA（各向同性假设，`i_d = 0`）；
- 高调制区两相重构（V1 以调制限幅保证采样窗口，见 §3.5）；
- 死区补偿、齿槽补偿、解耦前馈默认关闭（结构预留）；
- flash 持久化（V2 随参数持久化一并接入）。

---

## 2. 总体架构与分层

### 2.1 目录布局

```
App/Algorithm/FOC/                     ← 纯数学，零硬件依赖（无 hpm_*、无 Interface/Platform）
├── Inc/
│   ├── foc_math.h          ← [V1] Clarke/Park/反Park、角度 wrap、sincos（static inline）
│   ├── foc_angle.h         ← [V1] 机械角→电角度（p/方向/零点）+ ωe 估计
│   ├── foc_current.h       ← [V1] d/q 电流调节器（PI + 圆形电压限幅 + 抗饱和 + 前馈结构）
│   ├── foc_modulation.h    ← [V1] min-max 零序注入 + 调制限幅 + 占空比生成
│   └── id_encoder.h        ← [V1] 电角度辨识（lock-in + 扫描 + sin/cos 累加）
│   ── V2 预留（本阶段不建文件）──
│   ├── id_rs.h / id_ls.h / id_flux.h / id_tune.h / id_common.h
│   └── foc_limits.h（弱磁/电流矢量约束）/ foc_observer.h（无感占位）
└── Src/                   ← 同名 .c（foc_math 为纯 inline，无 .c）
    ├── foc_angle.c  foc_current.c  foc_modulation.c  id_encoder.c

App/Control/                            ← 编排（唯一硬件访问者）
├── app_foc.{h,c}           ← [V1] 状态机 + 模式 + 联锁 + 参数消费 + 25kHz 入口
├── app_foc_current.{h,c}   ← [V1] 25kHz 执行：读输入 → 调 Algorithm/FOC → 写逆变桥
└── app_motor_identify.{h,c}← [V1] 辨识编排（job 框架；V1 = encoder 辨识 + 结果验证/落库）
```

**V1 相对初版提案的收敛**：`foc_limits` 并入 `foc_current`（V1 只有圆形电压限幅与电流矢量限幅，无弱磁）；`foc_math` 用 `static inline`（热路径零调用开销，与 `algo_*_step_fast` 同风格）。

### 2.2 依赖方向

```
Application/Debug/Comm ──► Control ──► Platform ──► Interface ──► Driver ──► Board
                             │
                             └──► Algorithm/FOC（纯数学；只依赖 <math.h>/<stdint.h>/<stdbool.h>）
```

- `Algorithm/FOC/*` 不得包含 `Interface/`、`Platform/`、`hpm_*`（与现有 `algo_*` 同纪律）；
- 辨识模块不接触硬件：以**"激励请求 + 测量回喂"**纯状态机形式实现（§5.3）；
- 所有 FOC 相关硬件动作（读编码器/电流、写占空比、使能桥）只在 Control/Platform。

### 2.3 执行上下文与数据流

```
┌─ 25kHz 节拍（app_logic 主循环；与 PWM 同频）─────────────────────────┐
│ app_debug_encoder_sample()  → 转子编码器单次读取（Platform 缓存，~5µs）│
│ app_analog_signal_process() → 三相电流换算（无滤波）                  │
│ app_fault_process()         → L2 RMS / L3 / 健康（状态更新）          │
│ app_foc_run_once()          → 角度链 → 电流环 → 调制 → 占空比写入     │
│   └─ 读：Platform 缓存角度 / 电流 / VBUS / fault 状态                 │
│   └─ 调：foc_angle / foc_current / foc_modulation（Algorithm/FOC）    │
│   └─ 写：app_3phase_inverter_set_duty_abc()                          │
│ app_debug_motor_run_once()  → 仅当 FOC 未启用（互斥，§4.4）           │
└──────────────────────────────────────────────────────────────────────┘
        ▲ 给定/回读                          │ 1kHz 慢任务
┌───────┴────────────────────────────────────┴─────────────────────────┐
│ app_adc_slow_process / app_fault L3 / app_terminal_run_once（命令、   │
│ job tick、monitor）/ app_motor_identify_run_once（辨识进度与中止）     │
│ 外环（V2+）在此按分频系数运行，或随 25kHz tick 分频（§8.4）            │
└──────────────────────────────────────────────────────────────────────┘
```

**同拍保证**：V1 中角度、电流、电流环计算、占空比写入全部在同一 25kHz 迭代内完成，无跨上下文数据共享（无临界区、无原子性要求）。

---

## 3. Algorithm/FOC 设计（电机控制大类）

### 3.1 公共约定

- 风格沿用工程算法库：对象 = `xxx_ctor()` + 匿名函数指针表（`init/step/reset/get_*`），私有字段 `_` 前缀；
- 浮点域统一 `float`；热路径函数用 `ALGO_ATTR_RAMFUNC`（`.fast`/ILM），默认开启（与 `algo_pid` 的 `ALGO_ENABLE_ILM` 一致）；
- 输入防御：非有限值（NaN/Inf）→ 保持上一步输出并置内部 `_fault` 标志（不 trap、不静默输出 0）；
- 所有角度接口统一 [rad]；速度 [rad/s]；电流 [A]（dq 幅值 = 相电流峰值）；电压 [V]；
- 无动态分配、无 printf、单次 `step()` 有界（目标 ≤ 1µs @480MHz）。

### 3.2 `foc_math.h`（static inline）

**符号与坐标约定**（全文唯一来源）：

| 约定 | 定义 |
| :--- | :--- |
| 相序 | U → V → W；α 轴与 U 相轴重合 |
| Clarke（幅值不变，2/3 系数） | `iα = (2/3)(iu − iv/2 − iw/2)`；`iβ = (iv − iw)/√3` |
| Park | `id = iα·cosθe + iβ·sinθe`；`iq = −iα·sinθe + iβ·cosθe` |
| 反 Park | `vα = vd·cosθe − vq·sinθe`；`vβ = vd·sinθe + vq·cosθe` |
| 反 Clarke（调制用） | `vu = vα`；`vv = −vα/2 + (√3/2)vβ`；`vw = −vα/2 − (√3/2)vβ` |
| 电角度零点定义 | θe = 0 ⇔ 转子 d 轴（磁极轴）与 α（U 相）轴重合 |
| 转矩（V2 接口） | `T = kt_nm_per_a · iq`（手册 `kt = 0.117 N·m/A`，峰值电流口径；与手册转矩表自洽：0.117 × 24.3 ≈ 2.8 N·m）；`iq > 0` 产生使 θe 增大的转矩。V1 不暴露 N·m 接口 |

接口（示意）：

```c
static inline float foc_wrap_2pi(float x);          /* → [0, 2π) */
static inline float foc_wrap_pm_pi(float x);        /* → (−π, π] */
static inline void  foc_sincos(float theta, float* s, float* c);  /* V1: sincosf() */
static inline void  foc_clarke(float iu, float iv, float iw, float* ia, float* ib);
static inline void  foc_park(float ia, float ib, float theta, float* id, float* iq);
static inline void  foc_inv_park(float vd, float vq, float theta, float* va, float* vb);
static inline void  foc_inv_clarke(float va, float vb, float* vu, float* vv, float* vw);
```

- `foc_sincos` V1 直接用 `sincosf()`（480MHz + 单精度 FPU，先实测开销；若电流环预算超限，V2 换 1024 点 1/4 波表 + 线性插值，接口不变）；
- 三相重构（两相求和反推第三相）V2 随高调制区实现，V1 不做。

### 3.3 `foc_angle`（电角度链）

```c
typedef struct {
    uint8_t pole_pairs;         /* p（motor.pole_pairs） */
    int8_t  direction;          /* +1 / −1（辨识结果） */
    float   offset_rad;         /* 电角度零点（辨识结果，[0,2π)） */
    float   speed_lpf_hz;       /* ωe 估计低通截止（默认 100Hz） */
    float   sample_time_s;      /* 1/25000 */
} foc_angle_cfg_t;

/* step 输入：机械角 raw（未加软件零点的编码器原值，rad [0,2π)） */
float foc_angle_step(foc_angle_t* self, float theta_m_raw_rad, float* omega_e_out);
```

- 电角度：`θe = wrap_2pi( p · dir · θm_raw − offset_rad )`；
- **必须使用未加软件零点的原始角**：用户 `enc zero` 只影响显示/位置环，不得影响换相（否则零点变更会破坏已辨识的电角度零点）；
- ωe：由 θe 差分（`wrap_pm_pi(θe − θe_prev)/dt`）经一阶低通（默认 100Hz）→ 自动包含方向符号，供前馈/显示使用；
- 首拍/复位后 ωe = 0，避免差分冲激。

### 3.4 `foc_current`（d/q 电流调节器）

```c
typedef struct {
    float kp;              /* V/A（control.current_loop.kp） */
    float ki;              /* V/(A·s)（control.current_loop.ki） */
    float sample_time_s;   /* 40µs */
    uint8_t decoupling_en; /* 解耦前馈开关（V1 默认 0） */
    float l_d, l_q, lambda;/* 前馈模型（手册值；V2 辨识更新） */
    float aw_decay;        /* 饱和时积分衰减系数（默认 0.99；1.0 = 冻结） */
} foc_current_cfg_t;

typedef struct {
    float i_d_ref, i_q_ref;      /* 给定（A） */
    float i_d_a, i_q_a;          /* 反馈（A） */
    float v_bus_v;               /* 母线（V） */
    float v_max;                 /* 电压矢量限幅（每拍现算：§3.5 推导，随实测 VBUS） */
    float i_max;                 /* 电流矢量限幅（= i_q_max_a，峰值口径） */
    float omega_e_rad_s;         /* 前馈用（rad/s） */
} foc_current_in_t;

typedef struct {
    float v_d, v_q;              /* 限幅后电压（V） */
    float i_d_ref_lim, i_q_ref_lim; /* 限幅后给定（电流矢量限幅） */
    bool  saturated;             /* 本拍电压饱和 */
} foc_current_out_t;
```

**算法（每拍）**：

1. **给定限幅**（圆形电流矢量）：`|i_dq_ref| > i_max` → 按比例缩放（保角）；
2. **误差与 PI**：`ed = id_ref − id`，`eq = iq_ref − iq`；`vd = kp·ed + ∫d`，`vq = kp·eq + ∫q`；
3. **前馈**（`decoupling_en`）：`vd_ff = −ωe·Lq·iq`，`vq_ff = +ωe·(Ld·id + λ)`；加到 PI 输出（V1 关闭，模型用手册值）；
4. **圆形电压限幅**：`mag = √(vd²+vq²)`；`mag > v_max` → 缩放 `v_max/mag`（保角），`saturated = true`；
5. **抗饱和**（跨轴协调）：未饱和 → 积分累加 `∫ += ki·dt·e`；饱和 → 积分**衰减** `∫ *= aw_decay`（默认 0.99，可设 1.0 = 冻结）；同时每轴积分单独限幅 `|∫| ≤ v_max`；
6. **输出** `v_d/v_q`（已限幅）→ 供 `foc_modulation`。

**增益初值（手册 R/L + 目标带宽 ωbw = 2π·1kHz）**：`kp = Ls·ωbw ≈ 0.745 V/A`；`ki = Rs·ωbw ≈ 993 V/(A·s)`（极点对消法，与 ODrive `current_control_bandwidth` 自动整定、VESC `kp=l·bw; ki=r·bw` 一致）。V2 由 `id_rs/id_ls/id_tune` 实测更新。

**饱和语义**：V1 仅记录 `saturated` 供诊断（monitor/`foc` 命令显示）；是否需要限压降额/报警由后续保护动作层决定。

### 3.5 `foc_modulation`（min-max 零序注入）

```c
typedef struct {
    float duty_max;   /* 0.885（采样窗口约束，control.limits.duty_max） */
    float v_bus_min;  /* 低于此值拒绝输出（默认 9V，复用 fault.vbus_uv_v 量级） */
} foc_modulation_cfg_t;

int foc_modulation_step(const foc_modulation_cfg_t* cfg,
                        float v_alpha, float v_beta, float v_bus_v,
                        float duty_abc[3], float* v_scale_out);
```

**步骤**：

1. 反 Clarke 得 `vu/vv/vw`（V，相电压参考）；
2. **调制限幅**（保证三电阻采样窗口）：计算 `span = max(vu,vv,vw) − min(vu,vv,vw)`；约束 `span ≤ (2·duty_max − 1)·v_bus`（duty_max=0.885 → 系数 0.77）；超限则 `k = 0.77·v_bus/span` 缩放 `vα/vβ` 后重算（保角、保持线性）；
3. **零序注入**：`offset = −(vmax+vmin)/2`；
4. **占空比**：`d_x = 0.5 + (v_x + offset)/v_bus`；理论上有 `max(d)+min(d) = 1`（注入后天然成立）；
5. 逐相钳位到 `[1−duty_max, duty_max]` 作为**最后防线**（正常不触发；触发即记 `v_scale_out < 1` 供诊断）；
6. `v_bus < v_bus_min` → 返回 −1（Control 停止调制，零矢量）。

**限幅推导（记录在案）**：dq 矢量幅值 A（= 相电压峰值）与三相占空比跨度关系 `span_duty = 1.5·A/v_bus`；取 `span_duty ≤ 2·(duty_max−0.5) = 0.77` → **`v_max = (2·duty_max − 1)·v_bus / 1.5 ≈ 0.513·v_bus`**（24V → 12.3V）。该值为 `foc_current` 的圆形限幅上限（由 Control 每拍按实测 VBUS 计算下发）。

**说明**：V1 在 duty_max=0.885 内工作（线性区 + 采样安全）；由此损失的调制范围约 11%（相对 SVM 线性区 0.577·v_bus）。高调制区两相重构为 V2 项（ADC spec §10.5 已预留）。

### 3.6 无感占位（不建文件）

`foc_observer` 规划接口：输入 `vα/vβ`、`iα/iβ`，输出 `θe_est/ωe_est`，与 `foc_angle` 同为角度源候选（`app_foc` 的 `angle_source` 枚举预留 `APP_FOC_ANGLE_OBSERVER`）。V1 不实现。

---

## 4. Control 层设计

### 4.1 `app_foc`（状态机 + 策略）

```c
typedef enum {
    APP_FOC_STATE_OFF = 0,   /* 输出关闭（PWM 停 / 桥未使能） */
    APP_FOC_STATE_READY,     /* 桥已使能，零矢量（duty=0.5），i_dq=0 */
    APP_FOC_STATE_RUN,       /* 电流闭环 */
    APP_FOC_STATE_CALIB,     /* 辨识模式（强制角 + 辨识模块驱动） */
    APP_FOC_STATE_FAULT,     /* 故障门控：零矢量 + 关桥，等待清除 */
} app_foc_state_t;

typedef enum { APP_FOC_ANGLE_ENCODER = 0, APP_FOC_ANGLE_FORCED } app_foc_angle_source_t;
```

公开 API（示意）：

```c
void app_foc_init(void);
void app_foc_run_once(void);                 /* 25kHz 节拍 */
int  app_foc_enable(void);                   /* OFF → READY：检查 fault/编码器/参数 → 使能桥 */
void app_foc_disable(void);                  /* 任意状态 → OFF：停调制 + 关桥 */
int  app_foc_set_iq_ref(float i_q_a);        /* 转矩给定（限幅 ±i_q_max） */
int  app_foc_set_id_ref(float i_d_a);        /* 辨识/调试（默认 0） */
int  app_foc_set_angle_source(app_foc_angle_source_t src, float theta_e_rad);
app_foc_state_t app_foc_get_state(void);
bool app_foc_is_active(void);                /* != OFF（供 Debug/Comm 互斥判断） */
void app_foc_get_snapshot(app_foc_snapshot_t* out); /* θe/ωe/id/iq/vd/vq/duty/saturated/状态 */
```

**状态迁移与联锁**：

| 迁移 | 条件 | 动作 |
| :--- | :--- | :--- |
| OFF → READY | 显式 `enable()`；`fault == NORMAL`；`app_adc_is_valid()`；编码器最近读成功；参数合法（p>0、duty_max∈(0.5,1)） | 先 `app_3phase_inverter_enable()`，输出零矢量，`i_dq_ref = 0` |
| READY → RUN | 首次下发非零 `i_dq_ref`（RUN 中给定回零仍保持 RUN，等价零电流闭环） | — |
| RUN → CALIB | `app_motor_identify` 请求（`cal encoder`） | 强制角源；`i_d_ref = I_cal`、`i_q_ref = 0` |
| CALIB → RUN/OFF | 辨识完成/失败/中止 | 恢复角源与给定；失败时回 OFF |
| 任意 → FAULT | `app_fault_get_state() != NORMAL`（每拍检查） | 立即零矢量（duty=0.5/0.5/0.5）+ `app_3phase_inverter_disable()`；记录原因 |
| FAULT → OFF | `app_fault_clear()` 成功且状态回 NORMAL | 需显式 `enable()` 重新进入 |

**注**：故障动作层（L1 快跳 `force_low` 等）按 Kaiser 决策后置；V1 的安全停止 = 零矢量 + 关桥（软件路径，µs 级但非硬件级），并在 spec 风险项中标注。

### 4.2 `app_foc_current`（25kHz 执行）

每拍动作（纯执行，无策略）：

1. 读输入：转子 raw 角度（Platform 缓存）、`i_u/i_v/i_w`（`app_analog_signal_read`）、`v_bus`、`fault` 状态；
2. 数据有效性：任一电流为 NAN 或 ADC 未就绪 → 置 `fault` 内部标志并输出零矢量（不输出随机电压）；
3. 角度：`angle_source == ENCODER` → `foc_angle_step()`；`FORCED` → 直接用强制角（ωe 仍由 θe 差分估计，用于前馈/显示）；
4. 电流环：`foc_current_step()`（输入含 `v_max`，由 VBUS 与 duty_max 现算）；
5. 调制：`foc_modulation_step()` → `duty_abc[3]`；
6. 写桥：`app_3phase_inverter_set_duty_abc()`；
7. 快照：更新 `g_foc_*` 观测变量（`.noncacheable.bss`，供 Ozone）。

**调度**：由 `app_foc_run_once()` 统一调用；`app_foc_state == OFF/FAULT` 时跳过并输出零矢量/不写桥。
**例外（V1 实施补充）**：OFF 状态仍刷新快照的 θe/ωe（只读编码器 + 角度链，不写桥），
供台架"静态链路检查"（§9.1 步骤 1）在未使能时观察角度连续性。

### 4.3 参数消费

| 参数 | 来源 | 语义 |
| :--- | :--- | :--- |
| `motor.pole_pairs` | motor 域 | 电角度换算（**init 期消费**；运行中修改需重新 `app_foc_init()`） |
| `motor.encoder.electrical_offset_rad` / `direction` | motor 域（新增） | 辨识结果（V1 RAM，flash V2） |
| `control.current_loop.kp` / `ki` | software 域（live） | 电流环增益（在线可调） |
| `control.current_loop.bandwidth_rad_s` | software 域（新增，live） | 目标带宽（V1 仅文档/自整定用） |
| `control.current_loop.decoupling_en` | software 域（新增，live） | 解耦前馈开关（默认 false） |
| `control.limits.i_q_max_a` | software 域（live） | 电流矢量限幅（峰值口径，24.3A） |
| `control.limits.duty_max` | software 域（live） | 调制上限（0.885，采样窗口） |

- 每拍经 `app_*_params_current()` 读取（live 参数热更新，改动在下一拍生效）；
- `_mutable()` 仅 Terminal `param` 命令写入（既有纪律）。

### 4.4 互斥与联锁

| 场景 | 规则 |
| :--- | :--- |
| FOC vs 开环 V/F（`app_debug_motor`） | 互斥：FOC 非 OFF 时 `motor` 命令拒绝启动 V/F；V/F 运行中 FOC `enable()` 拒绝 |
| FOC vs `cal current`（电流零点标定） | 标定要求桥静止/零矢量：FOC 非 OFF 时拒绝标定；标定中 FOC 拒绝使能 |
| FOC vs `inv` 命令（逐相调试） | `inv` 命令要求 FOC OFF |
| Terminal 命令执行时间 | FOC 已启用时，命令单次处理仍 ≤200µs 预算（既有约束）；辨识走 job，不阻塞 |
| 参数写入（`param set`） | 增益类 live 参数允许运行中写；`duty_max` 下一拍生效；`pole_pairs` 为 init 期消费（运行中写需重新 `app_foc_init()`） |

---

## 5. 电角度辨识（V1 核心）

### 5.1 原理

电角度零点 = 转子 d 轴与 α（U 相）轴重合时的编码器机械角对应值。辨识方法（VESC `mcpwm_foc_encoder_detect` 同源思路，适配绝对编码器）：

1. **Lock-in**：强制电角度 θapp = 0，施加 `i_d = I_cal`、`i_q = 0` → 转子被拉到 θe = 0（d 轴对准 α 轴）；
2. **方向判定**：θapp 步进 +60° 电角度，测量编码器机械角变化 Δθm；`Δθm > 0 → dir = +1`，否则 −1；
3. **扫描 + 累加**：θapp 从 0 扫到 2π（正反各一遍），每步稳定后计算
   `δ = wrap_pm_pi( θapp − p·dir·θm_raw )`，累加 `Σsin(δ)`、`Σcos(δ)`；
4. **解算**：`offset_rad = wrap_2pi( −atan2(Σsin, Σcos) )`；质量指标 `q = |Σ|/N`（1.0 = 完美跟踪）；
5. **极对数校验**：一个电周期扫描中转子机械位移应为 `2π/p`（36°）；偏差 > ±20% → 报"极对数或传动比不匹配"警告；
6. **闭环验证**（Control 侧，§5.4）：用辨识结果做正常闭环锁定，残差统计。

**推导（符号约定）**：设真实关系 `θe_true = p·dir·θm_raw − offset`。Lock-in/扫描时转子跟随 θapp（θe_true = θapp），故 `δ = θapp − p·dir·θm_raw = −offset` → `offset = −atan2(Σsin, Σcos)`。符号最终以 §9 台架"闭环锁定验证"为准。

### 5.2 阶段与时间

| 阶段 | 激励 | 时长/步数 | 说明 |
| :--- | :--- | :--- | :--- |
| ARM | 无（检查） | — | fault NORMAL、编码器健康、FOC 状态 READY、I_cal 合法 |
| LOCK_IN | θapp = 0，`i_d = I_cal` | 500 ms | 等待转子稳定（打印警告：电机须自由旋转） |
| DIR | θapp = +π/3 | 300 ms | 测 Δθm 判方向 |
| SWEEP_FWD | θapp: 0 → 2π | 180 步 × 5 ms | 每步稳定后累加 |
| SWEEP_REV | θapp: 2π → 0 | 180 步 × 5 ms | 反向，抵消迟滞/摩擦偏差 |
| COMPUTE | — | — | offset / dir / q / 极对数校验 |
| DONE | — | — | 输出结果，Control 进入验证 |

总时长 ≈ 3.2 s（+ 验证 0.5 s）。

**驱动方式**：`id_encoder_step()` 由 **25kHz 电流环上下文**调用（`dt = 40µs`），内部按时长推进阶段、平滑斜坡输出 θapp（避免阶跃引起的电流冲击）；1kHz job 仅轮询进度/中止。辨识期间角度源 = FORCED（`θe = θapp`），电流环照常运行（`i_d_ref = I_cal`）。

### 5.3 接口（纯状态机）

```c
typedef enum {
    ID_ENCODER_PHASE_IDLE = 0, ARM, LOCK_IN, DIR, SWEEP_FWD, SWEEP_REV, COMPUTE, DONE, FAILED
} id_encoder_phase_t;

typedef struct {
    float theta_m_raw_rad;   /* 机械角（未加软件零点） */
    float i_d_a, i_q_a;      /* 电流反馈 */
    float v_bus_v;
    float dt_s;              /* 40µs */
} id_encoder_in_t;

typedef struct {
    float theta_e_cmd;       /* 强制电角度（Control 用作角度源） */
    float i_d_ref, i_q_ref;  /* 电流给定（Control 下发给电流环） */
    id_encoder_phase_t phase;
    float progress;          /* 0..1 */
    /* 结果（COMPUTE 后有效） */
    float offset_rad;
    int8_t direction;
    float quality;           /* |Σ|/N */
    float mech_ratio_err;    /* 极对数校验偏差 */
    bool done;
    bool failed;
} id_encoder_out_t;

typedef struct { /* 配置：I_cal、步数、步时长、方向判定角、质量阈值、超时 */ } id_encoder_cfg_t;

void id_encoder_ctor(id_encoder_t* self);
int  id_encoder_init(id_encoder_t* self, const id_encoder_cfg_t* cfg);
void id_encoder_step(id_encoder_t* self, const id_encoder_in_t* in, id_encoder_out_t* out);
void id_encoder_reset(id_encoder_t* self);
```

### 5.4 编排与验证（`app_motor_identify`）

- **入口**：Terminal `cal encoder`（job 框架：单前台、进度显示、任意键中止）；
- **前置**：FOC 已 `enable()` 且状态 READY；fault NORMAL；编码器错误计数不增长；打印安全警告（自由旋转、电流大小、预计时长）；
- **过程**：设置 `angle_source = FORCED`、`i_d_ref = I_cal` → 每拍调用 `id_encoder_step()` → 进度显示（阶段 + %）；
- **验证（Control 侧，辨识完成后）**：
  1. 把结果写入 `foc_angle` 配置（offset/dir，RAM）；
  2. 恢复 `angle_source = ENCODER`，保持 `i_d = I_cal`、`i_q = 0` 500ms；
  3. 统计残差 `δ = wrap_pm_pi(θe_enc)`（应 ≈ 0）：**均值 |δ| < 5° 且 最大 |δ| < 15° → PASS**；否则 FAIL（提示：机械松动/编码器方向/极对数/电流过小）；
  4. 可选方向验证：`i_q = +1A` 短暂施加，确认转子运动方向与 θe 增大方向一致（1s 内不判失败，仅记录）；
- **结果**：PASS → 写 `motor.encoder.electrical_offset_rad` / `direction`（RAM，打印）；FAIL → 保留原值，打印失败原因与建议；
- **中止**：任意键 → 停止激励（`i_dq_ref = 0`）、恢复角源、状态回 OFF。

### 5.5 安全边界

| 项 | 值/规则 |
| :--- | :--- |
| 辨识电流 `I_cal` | 默认 **2.0 A**（峰值）；可配范围 (0, min(i_q_max, 5A)] |
| 功率上限 | `I_cal²·Rs·t` 极小（2A² × 0.158Ω × 3.2s ≈ 2.0 J）；仍记录为检查项 |
| 转子行程 | 电角度 2π = 机械 36°（p=10），无需整机旋转空间 |
| 超时 | **V1 实施偏差**：单一总超时 15s（`id_encoder.timeout_ms`）+ 编排侧 30s 兜底（1kHz tick）；每阶段独立超时（LOCK_IN 2s / DIR 1s / 每扫描 3s）为 v2 细化项（已记录，未实现） |
| 故障 | 辨识中 fault != NORMAL → 立即中止（走 FOC FAULT 路径） |
| 编码器 | 辨识中错误计数增量 > 0 → FAILED（ADC spec 健康项同源） |
| 温度 | NTC 换算未定；V1 仅打印 NTC 电阻供人工判断 |
| 自由旋转要求 | 命令打印警告；不强制机械确认（可加 `confirm` 参数，V1 不做） |

---

## 6. 平台与接口变更清单

| # | 位置 | 变更 | 理由 |
| :--- | :--- | :--- | :--- |
| 1 | `app_encoder` | 新增**转子共享采样**：`app_encoder_sample_rotor()`（25kHz 单次读取，~5µs）+ `app_encoder_get_rotor_raw(uint16_t*)`（缓存读取，无 I/O）+ `app_encoder_get_rotor_age()`（可选，诊断） | 避免 FOC 与 Debug 重复读 SPI（各 5µs）；单一所有者，无并发 |
| 2 | `app_debug_encoder_sample` | 转子改读共享缓存（输出轴保持原路径） | 同上，避免双读 |
| 3 | `app_fault` | 无变更（`app_fault_get_state()` 已够用） | — |
| 4 | `app_analog_signal` | 无变更（电流无滤波；`read()` 返回 NAN 的语义已有） | — |
| 5 | `app_3phase_inverter` | 无变更（`set_duty_abc` 已满足；零矢量 = 0.5/0.5/0.5） | — |
| 6 | `app_logic.c` | init 阶段新增 `app_foc_init()`；25kHz 节拍新增 `app_foc_run_once()`（在 `app_fault_process()` 之后、`app_debug_motor_run_once()` 之前） | 接线 |
| 7 | `CMakeLists.txt` | include 增加 `App/Algorithm/FOC/Inc`；源文件增加 `App/Algorithm/FOC/Src/*.c`、`App/Control/Src/app_foc*.c`、`app_motor_identify.c` | 构建 |
| 8 | `App/Comm/terminal` | 新增 `foc` 命令（`status/on/off`）；`motor` 增加 `iq <A>`；`cal` 增加 `encoder` 子命令 | 可观测可操作 |
| 9 | `App/Debug` | 新增 `app_debug_foc.{h,c}`（`foc` 命令数据源 + Ozone 快照刷新，可选） | 调试 |

**实施偏差记录（2026-09-21）**：
- monitor 常驻状态区**未增加** θe/ωe/id/iq 行（`foc status` 已提供完整快照，避免 monitor 行数膨胀）；
- `app_debug_foc`（调试数据源模块）未单独建立（`foc` 命令 + `g_foc_current_snapshot` 已覆盖）；
- `motor.encoder.electrical_offset_rad/direction` 的元数据 `apply` 为 LIVE，但实际在
  下一次 `app_foc_enable()` 时消费（`enable()` 内 `set_offset`）——已在此记录。

**PWM 影子寄存器行为记录**：`drv_hrpwm` 的 CMP 更新触发为 `pwm_shadow_register_update_on_modify`（SDK 语义：**写入即生效**，非周期锁存）。异步写入可能使当前周期脉冲边沿轻微抖动；现有 V/F 旋转（同为 25kHz 异步写入）台架未见异常。V1 沿用；ISR 迁移后写入位置固定在谷底后（余量最大），列为观察项（§10）。

---

## 7. 参数与 YAML 变更

`config/motor.yaml`：

```yaml
encoder:
  # ... 现有字段不变 ...
  electrical_offset_rad: 0.0    # 电角度零点 [rad]，由 cal encoder 写入（V1 RAM，flash V2）
  direction: 1.0                # 编码器方向（+1.0 / −1.0），由 cal encoder 写入
```

`config/software.yaml`：

```yaml
control:
  current_loop:
    kp: 0.745              # [V/A] = Ls × ωbw（手册值；V2 自整定更新）
    ki: 992.7              # [V/(A·s)] = Rs × ωbw
    bandwidth_rad_s: 6283.2  # 目标带宽 2π×1kHz（V1 文档/自整定用）
    decoupling_en: 0         # 解耦前馈（0/1；V1 关闭）
  limits:
    i_q_max_a: 24.3          # 电流矢量限幅（**峰值口径** = 相电流峰值；10s 峰值）
    duty_max: 0.885          # 调制上限（三电阻采样窗口约束，ADC spec §4）
```

**生成器登记（`scripts/gen_params.py` SCHEMA）**：生成器仅支持 `u8/u16/u32/f32`（无 bool/int 类型），新增字段按此登记：

| 字段 | 类型 | min/max |
| :--- | :--- | :--- |
| `motor.encoder.electrical_offset_rad` | f32 | 0, 2π |
| `motor.encoder.direction` | f32 | −1, 1（合法值仅 ±1.0，运行期校验） |
| `control.current_loop.bandwidth_rad_s` | f32 | 0, None |
| `control.current_loop.decoupling_en` | u8 | 0, 1 |

**口径修正（重要）**：

- 现有 `control.limits.i_q_max_a` 注释为"RMS 口径"，与 G66-18 手册 `I_peak(10s) 24.3A` 矛盾 → 统一为 **dq 矢量幅值 = 相电流峰值口径**（与 Clarke/Park 幅值不变约定一致）；`config/motor.yaml` 与 `app_motor_params.h` 中 `i_peak_10s_a` / `i_peak_2s_a` 的注释"（RMS）"同步修正为"（峰值）"；
- `duty_max` 由 0.95 改为 **0.885**（采样窗口），硬件层钳位仍为 [0,1]；
- 连续热限幅（7.0 A RMS = 9.9 A 峰值）V1 不实现（仅 10s 峰值限幅），V2 随温度/NTC 接入补充。

---

## 8. 实时性与时序预算

### 8.1 25kHz 预算（40µs/拍）

| 项 | 估计 | 备注 |
| :--- | :--- | :--- |
| 转子编码器 SPI（共享采样） | ≈5µs | 已有（Debug 也在用） |
| 三相电流换算 | <1µs | `app_analog_signal_process`（已有） |
| 故障处理 | <1µs | 已有 |
| 角度链 + sincos + Clarke/Park + PI + 限幅 + 反 Park + 调制 | **≈2µs（估）** | 待实测（`g_foc_loop_cycles`） |
| 占空比写入（3 对 CMP） | <0.5µs | — |
| **新增合计** | **≈2.5µs（估）** | 目标 <5µs；若超限先优化 sincos（查表） |

### 8.2 前置条件（FOC 台架前必须处理）

1. **主循环 4% 丢拍**（ADC spec §10.10，late≈936/s）：FOC 接入后周期抖动直接进入电流环 → 先定位（1Hz 打印路径/慢任务）并优化；
2. **ADC0 触发风暴**（ADC spec §9.4，间歇自愈）：FOC 前重测确认；若复现按 §9.4 取证流程处理；
3. 编码器共享采样改造完成（避免双读 10µs → 5µs）。

### 8.3 ISR 迁移路径（V1.5，非本次）

- `app_adc` 增加 `pmt_frame_cb`（PMT 帧完成回调，ISR 上下文）→ FOC 电流环迁入 ADC0 ISR（谷底+500ns 触发，采样-输出延迟确定）；
- 编码器读取与电流换算随迁（或保留主循环 + 角度外推）；
- 收益：采样-写 PWM 相位固定、免疫主循环抖动；代价：ISR 占用率上升（≈10µs/40µs 含 SPI），需临界区纪律。
- 本设计（`foc_angle/foc_current/foc_modulation` 纯函数、无上下文假设）对迁移无阻碍。

### 8.4 外环提频预留

外环（速度/位置）统一按 **电流环 tick 分频**调用：`divider = 25`（1kHz）为初值，未来提高到 5（5kHz）/ 1（25kHz）仅改参数与调度位置，不动算法结构（`algo_pid` 支持任意 `sample_time_s`）。

---

## 9. 验证计划

### 9.1 台架分步（每步判据）

| # | 步骤 | 操作 | 判据 |
| :--- | :--- | :--- | :--- |
| 1 | 静态链路 | FOC OFF；手动转动转子 | `foc` 显示 θe 连续、ωe 符号正确、无跳变；电流 ≈0 |
| 2 | 强制角锁 d 轴 | `cal encoder` 的 LOCK_IN 阶段或手动强制角 θe=0 + i_d=2A | 转子锁定；手感阻力；`i_q ≈ 0` |
| 3 | 电流阶跃 | `motor iq 2` / `motor iq 0` | 上升时间 < 2ms（目标带宽 1kHz）；超调 < 20%；无振荡/尖啸 |
| 4 | 转矩方向 | `motor iq 1` 与 `motor iq -1` | 正负给定对应相反转向；θe 单调变化 |
| 5 | 小电流旋转 | `motor iq 0.5`（自由轴） | 平稳旋转；`i_d` 波动 < 0.3A；电流波形正弦 |
| 6 | 电角度辨识 | `cal encoder` | 质量 q > 0.95；offset 重复性 ±2°（连测 3 次）；PASS 后闭环锁定残差 < 5° |
| 7 | 断电重测 | 重启后手动填 offset（RAM 丢失）→ 重辨识 | 两次 offset 一致（±2°） |
| 8 | 极限/保护 | 给定 > i_q_max；人为制造 fault（降压至 UV） | 给定被限幅；fault 后零矢量 + 关桥；清除后需重新 enable |
| 9 | 温升/纹波（可选） | 1A 连续 10min | 电流纹波/温升记录，供 V2 死区补偿/滤波评估 |

### 9.2 回归（不破坏既有功能）

- 开环 V/F（`motor`）在 FOC OFF 时行为不变；
- `cal current`、`inv`、`adc/pwm/enc/fault` 命令不变；
- 主循环 late 计数不因 FOC 接入明显恶化（对比接入前后 `late/s`）；
- `param` 命令对 `control.*` 的读写生效（live）。

### 9.3 回滚

- 软件：`app_foc` 默认不使能（上电 OFF），FOC 异常不影响 V/F 与其它功能；
- 参数：`control.*` 修改仅在 RAM（`param reset` 回工厂值）；
- 提交粒度：Algorithm/FOC 纯数学层 → Control 编排 → 辨识 → 平台改造，分 3~4 个 commit（便于二分回滚）。

---

## 10. 已知风险与后续项

| # | 项 | 类型 | 处理 |
| :--- | :--- | :--- | :--- |
| 1 | 主循环 4% 丢拍 → 电流环周期抖动 | 时序 | §8.2 前置；V1.5 ISR 迁移根治 |
| 2 | ADC0 触发风暴（间歇） | 硬件行为 | FOC 前重测确认；复现则按 ADC spec §9.4 取证 |
| 3 | PWM CMP `update_on_modify` 异步写入 → 边沿抖动 | 硬件行为 | 观察项（V/F 现状未见异常）；ISR 迁移后固定写入相位 |
| 4 | 电流采样窗口 d_max=0.885 → 调制范围 −11% | 设计取舍 | V1 接受；高调制区两相重构 V2 |
| 5 | `Ke` 与 `Kt` 手册值不自洽：KV70 折算 Ke=0.125 V·s/rad → λ≈0.0111 Wb → Kt≈0.167 N·m/A，与手册 Kt=0.117 相差 ≈1.4× | 数据 | V1 转矩口径取手册 Kt（与手册转矩表自洽：0.117×24.3≈2.8 N·m ✓）；V2 `id_flux` 实测统一 |
| 6 | 编码器安装松动/打滑 → 辨识偏移漂移 | 机械 | 验证残差 + 重复性检查；带载运行前复测 |
| 7 | 故障动作层未实现（V1 仅软件零矢量 + 关桥） | 安全 | Kaiser 决策后置；V1 风险明示；硬件保护（预驱/WDOG L1）仍在 |
| 8 | 辨识需要自由旋转 | 操作 | 命令警告 + 中止键；V2 增加 `confirm` 参数 |
| 9 | R/L/Ke 辨识与增益自整定缺失（V1 用手册值） | 范围 | V2：`id_rs/id_ls/id_flux/id_tune` |
| 10 | 无感未实现 | 范围 | 占位（`foc_observer` + 角度源枚举） |
| 11 | 速度/位置环未实现 | 范围 | V2/V3；外环分频点已预留 |
| 12 | 温度保护缺失（NTC 换算未定） | 数据 | 型号确认后补；V1 辨识打印 NTC 电阻人工判断 |
| 13 | `app_3phase_inverter_enable()` 含 ~10ms 阻塞（+12V 栅极稳定等待），`foc on`/`motor start` 期间 25kHz 环与 L2/L3 暂停 | 实时性 | 阻塞窗口内桥关闭、无电流风险；与既有 V/F 路径同构。**v2：非阻塞桥使能**（断言 +12V → 主循环 deadline 后启动 PWM） |
| 14 | 辨识为**单一总超时**（15s）+ 30s 编排兜底，未实现 spec §5.5 的每阶段独立超时 | 范围 | V1 接受（锁定态 2A 持续 15s 热效应可忽略）；v2 细化 |
| 15 | `motor.encoder.*` 参数元数据标 LIVE，实际在下一次 `app_foc_enable()` 消费 | 元数据 | V1 已记录（§6 偏差）；v2 可细化 apply 语义或改为运行期热更新 |
| 17 | 快速过流跳闸（`i_trip_a`，默认 10A）**低于**电流限幅 `i_q_max_a`（24.3A）：V1 bring-up 阶段有效电流上限 = 10A；超过将跳闸 | 设计取舍 | 有意为之（首轮台架保护弱电源）；需要更大电流前先上调 `i_trip_a`（LIVE）；量产建议 ≥1.2×`i_q_max_a`。跳闸未接入 `app_fault` 位图（无 first-fault 记录，仅 `foc status` 的 `trip=`/FAULT 态）——v2 接入 |
| 16 | **电流采样符号与马达约定反相**（低侧采样 `I_*+` 接 MOSFET 源极）——V1 首轮台架 `foc on` 即失控（母线跌落复位） | 硬件/软件约定 | 已修复：`hardware.current_sense.invert`（默认 1，LIVE）+ `app_analog_signal` 取反；详见 ADC spec §6。V/F 开环无电流反馈，无法发现该问题 |

---

## 11. 实施顺序（供计划拆分）

1. **P1a 纯数学层**：`foc_math / foc_angle / foc_current / foc_modulation` + 单元级自测（纯函数可离线验证：变换可逆性、限幅边界、wrap 边界）；
2. **P1b 电流环闭环**：`app_foc / app_foc_current` + 平台共享采样 + `app_logic` 接线 + `foc` 命令 + 参数接线 → 台架步骤 1~5；
3. **P1c 电角度辨识**：`id_encoder` + `app_motor_identify` + `cal encoder` → 台架步骤 6~7；
4. **P1d 保护/联锁收尾**：fault 门控、互斥规则、`param` 生效验证、回归 → 台架步骤 8~9；
5. 每个阶段独立 commit（中文正文 + 验证结论）。

---

## 12. 变更记录

| 日期 | 变更 | 说明 |
| :--- | :--- | :--- |
| 2026-09-21 | 初稿 | 决策记录见 §0（Kaiser）；V1 范围 = 电流环 + 电角度辨识；R/L/Ke 与无感后置 |
| 2026-09-21 | 台架修复（二轮） | **现象**：符号修复后 `foc on` 正常（电流≈0、无跳闸）；随后 `motor iq 2`（**电角度零点未标定**，offset=0）→ 转子快速旋转约 5s → 电流达 12.5A → 过流跳闸保护停机（无母线跌落、无复位）。**判读**：零点未标定时转矩以任意电角度施加 → 持续转矩 → 空载电机升速 → 高速下反电动势接近母线电压、电流环失去控制 → 电流冲高；跳闸按设计动作。**修复/改进**：① 新增转矩模式限速 `control.limits.speed_max_rad_s`（默认 2500 rad/s 电角，超限零转矩、非锁存）；② FOC/辨识的机械角换算改用编码器分辨率接口（去掉硬编码 16bit）；③ 修正磁链口径：新增派生参数 `motor.flux_linkage_wb = ke·√2/(√3·p)`（原把"机械角·线 RMS"的 ke 直接当前馈 λ 用，偏大 ≈11×；V1 前馈未启用，属潜在缺陷）。**待验证**：日志 `omega=4234 rad/s`（=4042 rpm）超出 24V 母线可达转速（≈1650 rpm），需台架核对角度链速度标度（V/F 已知频率对照） |
| 2026-09-21 | 台架修复 | **现象**：`foc on` 启动电流过大 → 母线跌落 → 芯片复位（高概率）。**根因**：电流采样符号与 FOC 马达约定反相（原理图核实：低侧采样 `I_*+` 接 MOSFET 源极、`I_*−` 接 PGND，运放 +IN 接 `I_*+` → 测得电流为"流出电机"方向）→ 电流环成**正反馈**，零给定时亦指数发散（τ≈200µs，上限 v_max/R≈78A）。**修复**：`app_analog_signal` 按 `hardware.current_sense.invert`（默认 1，LIVE 可在线翻转）取反；**安全网**：新增快速过流跳闸 `control.limits.i_trip_a`（默认 10A，连续 2 拍 → 零矢量+关桥+FAULT，`foc status` 显示 `trip=`，`foc off` 恢复） |
| 2026-09-21 | V1 实施 | P1a~P1d 落地：Algorithm/FOC（foc_math/foc_angle/foc_current/foc_modulation/id_encoder）+ Control（app_foc/app_foc_current/app_motor_identify）+ Terminal（foc/motor iq/cal encoder）+ 参数登记；主机自测 252 用例；台架待按 §9 执行 |
