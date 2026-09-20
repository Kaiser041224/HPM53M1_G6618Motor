# 故障保护与错误处理设计（v1：纯判断）

> 状态：设计已评审确认（2026-09-19），待实现
> 前置事实：本板**无硬件 FAULT 输入**（板级设计已确认，见 bring-up 设计 §8）——软件保护是第一道防线。

## 1. 目标与范围

**目标**：在 App 层建立独立的故障保护与错误处理模块——故障检测、故障码、状态机；
为后续动作层（快跳/停机）提供统一的故障源与状态。

**v1（本次实现）**：

- 三级过流保护：L1 硬件 WDOG 快判（µs 级）/ L2 RMS 慢判 / L3 母线欠压过压；
- 链路健康：ADC 帧超时、编码器读失败（"静默失效"类）；
- 故障码输出（`f` 命令 + Ozone 观测变量）+ 锁存与清除；
- **只判断，不动作**——桥臂/PWM/电机行为完全不受影响。

**v2（后续）**：动作层——L1 快跳（`force_low`）、L2 降额/停机、故障恢复流程、YAML 阈值管线。

## 2. 架构与分层

- 新增 `App/Control/` 层（AGENTS.md §2.2：保护策略属控制语义；FOC 控制器后续同层）；
- `app_fault.{h,c}`：故障检测 + 状态机 + 故障码（只依赖 Interface 与 App/Platform）；
- `App/Debug/app_debug_fault.{h,c}`：`f`（打印）/ `F`（清除锁存）命令；
- 接线由 `App/Logic`（app_logic）编排：初始化顺序、回调注册、节拍调用。

依赖方向：`Logic → Control(app_fault) → Platform(app_analog_signal / app_adc / app_encoder) → Interface → Driver`。

## 3. 故障码表（32 位位图）

| 位 | 故障码 | 级别 | 判据 | 去抖 |
|---|---|---|---|---|
| 0/1/2 | `APP_FAULT_OC_FAST_U/V/W` | L1 | ADC WDOG 硬件阈值（±72.9A） | 无（硬件立即） |
| 3/4/5 | `APP_FAULT_OC_SLOW_U/V/W` | L2 | RMS > 72.9A | 连续 5 次（1kHz） |
| 6 | `APP_FAULT_VBUS_OV` | L3 | V_VBUS > 36V | 连续 5 次 |
| 7 | `APP_FAULT_VBUS_UV` | L3 | V_VBUS < 9V | 连续 5 次 |
| 8 | `APP_FAULT_ADC_TIMEOUT` | 健康 | PMT 帧序号停滞 > 10ms | 无 |
| 9 | `APP_FAULT_ENC_READ` | 健康 | 编码器错误计数增量 ≥ 3 | 无 |
| 10-31 | 预留 | — | 温度等 | — |

## 4. 状态机

```
INIT → NORMAL ⇄ WARNING → FAULT（锁存） → [F 清除 + 条件已恢复] → NORMAL
```

- `WARNING`：L2/L3 越限但未达去抖次数（计数中；条件恢复即回 NORMAL）；
- `FAULT`：L1 立即 / L2/L3 达去抖 / 健康类触发；**锁存**（条件消失亦保持）；
- 清除（`F`）：仅当所有条件已恢复时允许；否则拒绝并提示；
- 每次进入 FAULT：记录**首故障**（首因）、故障计数、**触发快照**（相关数值）；
- **动作钩子（v2）**：状态迁移处预留空实现位，v1 不接任何动作。

## 5. 检测数据流

| 级别 | 位置 | 路径 |
|---|---|---|
| L1 | ADC0 WDOG 中断 | PMT 通道 raw 越界 → `INT_STS[ADWDGx]` → ADC0 ISR → `adc_wdog_cb` → `app_fault_on_wdog()` |
| L2 累加 | 25kHz 主循环 | `app_fault_process()`：读三相电流 → `algo_rms`（窗口 250 = 10ms，覆盖 ≥5 个电周期） |
| L2 判断 | **1kHz ADC1 完成回调** | `app_fault_tick()`：RMS 阈值比较 + 去抖 |
| L3 | 同上 | `app_analog_signal_read(V_VBUS)` → 9V/36V 比较 |
| 健康 | 同上 | `app_adc_get_sequence()` 停滞 / `app_encoder_get_error_count()` 增量 |

- ADC1 完成回调：`app_adc_cfg_t` 增 `slow_cb`（app_adc 内部适配为驱动 `seq_cb`，启用 SEQ 完成中断）；
  驱动**仅使能整帧完成中断（SEQ_CMPT）**，回调每帧一次；
- ISR 上下文约束：tick 只做阈值比较/计数/记录（无打印、无阻塞）。

## 6. 接口

```c
/* App/Control/app_fault.h */
typedef struct {
    float    oc_fast_a;     /* L1 相电流阈值 [A]，0 = 默认 72.9A */
    float    oc_slow_a;     /* L2 RMS 阈值 [A]，0 = 默认 72.9A */
    float    vbus_ov_v;     /* 过压 [V]，0 = 默认 36V */
    float    vbus_uv_v;     /* 欠压 [V]，0 = 默认 9V */
    uint16_t slow_debounce; /* L2/L3 去抖次数，0 = 默认 5 */
    uint16_t adc_stall_ms;  /* PMT 帧超时 [ms]，0 = 默认 10 */
    uint8_t  enc_err_delta; /* 编码器错误增量阈值，0 = 默认 3 */
} app_fault_cfg_t;

void     app_fault_init(const app_fault_cfg_t *cfg);   /* NULL = 全默认 */
void     app_fault_process(void);                      /* 25kHz：RMS 累加 */
void     app_fault_tick(void);                         /* 1kHz：L2/L3/健康判断（ISR） */
void     app_fault_on_wdog(adc_channel_t ch, uint16_t value, void *user); /* L1（ISR） */
int      app_fault_clear(void);                        /* 清除锁存（条件须已恢复） */
uint32_t app_fault_get_state(void);
uint32_t app_fault_get_codes(void);                    /* 当前有效故障位图 */
uint32_t app_fault_get_latched(void);                  /* 锁存故障位图 */
uint32_t app_fault_get_first(void);                    /* 首故障码 */
```

- `app_adc_cfg_t` 扩展：`wdog_en / wdog_thshd_high / wdog_thshd_low / wdog_cb / wdog_cb_user`、`slow_cb`；
- 驱动扩展（`drv_adc`）：
  - PMT 分支支持 WDOG（逐通道阈值 + INT_EN + wdog 状态注册）；
  - WDOG 值源 `BUS_RESULT` → `PRD_RESULT`（全模式通用；PMT 更新 PRD_RESULT 已实测确认）。

## 7. 阈值（默认值，2026-09-19 确认）

| 项 | 值 | 依据 |
|---|---|---|
| L1/L2 相电流 | **72.9A = 3 × 24.3A** | 电机手册 `I_peak(10s) 24.3A`（G66-18）的 3 倍；量程 ±100A 内 |
| L3 母线 | **OV 36V / UV 9V** | 24V 系统 |
| 去抖 | L2/L3 连续 5 次（5ms）；健康项即时 | 工程初值 |
| WDOG raw 窗口 | [11051, 54483]（中值 32767 ± 72.9A × 297.9 counts/A） | 标称零点；逐相标定后重装为 v2 精化项 |

## 8. 输出

- `f`：状态 + 有效/锁存故障位解码 + 首故障 + 各故障计数 + 触发快照；
- `F`：清除锁存（条件须已恢复）；
- Ozone：`g_fault_state` / `g_fault_codes` / `g_fault_latched`（`.noncacheable.bss`）。

## 9. 验证方法

1. **L1 台架自测**：临时将 WDOG 下限设为高于静态 raw（如 `THSDL=33000`，静态≈32768）→ 应立即记录
   `OC_FAST_*` → 恢复真实阈值（**关键验证点：WDOG 在 PMT 模式下的硬件行为**）；
2. **L2/L3**：临时小阈值注入（如 RMS 2A、母线 20V）→ 观察 WARNING→FAULT 迁移与去抖行为；
3. **健康**：编码器错误注入（断开一路 SPI 读数）；ADC 停滞难以主动模拟（记录为待机验证项）；
4. **全程回归**：确认桥臂输出/旋转/电流链行为与保护前一致（纯判断无动作）。

**验证记录（2026-09-19/20）**：

- 启动/基线：`f` → `state=NORMAL codes=0`，计数全 0 ✓
  （修复记录：首版因缺 `algo_rms_ctor()` 调用导致 NULL 函数指针 trap——已修复；
  tick 的"按帧去重"曾使 ADC 停滞检测成为死代码——已修复为驱动每帧单次回调）；
- 旋转回归：f=1.0~5.0Hz 旋转正常、三相电流/VBUS/慢通道正常、`f` 保持 NORMAL ✓；
- **手动欠压实测（2026-09-19/20）**：降压至 8.88V → 连续去抖后 `FAULT`（`codes=0x80`、快照 vbus=8.88V）✓；
  条件未恢复时 `F` → `REJECTED`（门控正确）✓；电压恢复后 `active` 自动解除、`latched` 保持 ✓；
  条件恢复后 `F` → `clear OK`，状态/锁存/首故障/快照全部归零 ✓
  （修正：事件计数与编码器错误基线原先未随清除归零，已改为"F = 完整复位"）；
- **待验证（关键未知项）**：L1 WDOG 在 PMT 模式下是否触发（台架自测第 1 步）；
  L2 注入验证；编码器健康项。

## 10. 已知边界与后续项

- WDOG 在 PMT 模式下的硬件行为未实测（手册未排除）——台架自测确认；不工作则回退软件峰值比较（25kHz，40µs）；
- L2 阈值暂与 L1 相同（3×峰值）——热保护细调待电机实测数据；
- WDOG 阈值基于标称零点（32768）——逐相标定后的精确重装为 v2 精化项；
- **tick 依赖 ADC1 SEQ 链**：若 ADC1/GPTMR 触发失效，L2/L3/健康判断随之停止（盲区；仅 L1 硬件 WDOG 仍有效）
  ——v2 可加主循环侧心跳检测；
- **L1 WDOG 为一次性**：命中后驱动自动关闭该通道中断，仅 `F` 清除时重装——单次瞬态会暂时解除该通道硬件保护；
- **ENC_READ 为事件型**：无自然恢复信号，仅经 `F` 清除解除（其余条件恢复时自动解除 active）；
- **清除为临界区直执行**：`app_fault_clear()` 在主循环内以 `intf_sys_irq_save/restore`
  保护"复检 → 完整复位"，与故障 ISR 互斥（**注意：SDK ISR 使能嵌套，ADC0 WDOG
  优先级 2 > ADC1 1，ADC0 ISR 可抢占 ADC1 ISR——共享状态访问必须临界区**）；
- 动作层（v2）：L1 快跳 `force_low` + 故障锁存、L2 降额/停机、恢复流程；
- YAML 参数管线接入阈值（cfg 结构已预留）。
