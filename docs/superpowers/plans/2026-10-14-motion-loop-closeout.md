# Motion 力矩/速度环闭环可用性 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 补完力矩/速度环的闭环可用性缺口（坏拍有界保持、速度 PI 抗饱和、执行反馈诊断），使空载下 `motion torque`/`motion speed` 可正常使用并可测试。

**Architecture:** 采样/控制架构不变（PWM25kHz 双编码器取帧 → GPTMR1 CH1 10kHz motion）。motion 为唯一写者：坏拍从"一次即撤销"改为有界保持（零力矩、冻结积分、≤5 拍恢复，超限撤销）；速度 PI 以 FOC 执行反馈（accepted/limited/saturated/expired）做条件积分；诊断字段进 snapshot 与 `motion show`。

**Tech Stack:** C17 / HPM53M1 / FreeRTOS（不改）、宿主 mock 回归 `scripts/tests/{control,encoder}/run.sh`。

**Spec:** `docs/superpowers/specs/2026-10-14-motion-final-review.md`（未关闭项即本计划输入）；本计划的策略决策见 Global Constraints，均为 Mephisto 工程判断，用户可否决。

## Global Constraints

- 不改电流环数学、ADC/PWM/编码器触发链、`APP_PROTECT_ACTION_EN=0`、非向量/非抢占 FreeRTOS 配置。
- 不实现位置 P（下一轮）；不自动恢复超过保持窗口的失效（超限仍撤销，需新命令）。
- 坏拍有界保持策略（决策）：仅 SAMPLE/DUPLICATE/VERNIER/TIMING 四类且 active 时生效；
  保持期间每拍提交零力矩请求、冻结 PI 积分、清 `feedback_valid`/`g_motion_shaft_angle_valid`，
  保留 mode/target；`APP_MOTION_HOLD_MAX_SAMPLES=5`（10kHz 下 500µs，与反馈新鲜度界一致）；
  第 6 个连续坏拍按原语义撤销（reason=该类）。PARAMETER/FOC_STATE/SUBMIT 立即撤销，不做保持。
- 保持恢复后的首个好拍：跳过跨隙测速差分（`s_velocity_gap`），跳过 timing 拒绝检查，
  PI 用真实 dt（≤1ms，algo 自带 >1ms 返回 0 的防线）。
- 速度 PI 抗饱和（决策）：条件积分——`algo_motion_pi_t` 增加 `bool saturated`；
  饱和时仅误差反向（回退方向）允许积分，非饱和维持原条件（|raw|<limit 或反向）。
  motion 每拍读 `app_foc_get_execution()`：反馈属于本请求（valid/epoch/sequence 匹配）才采信；
  `current_limited || voltage_saturated || request_expired || 不属于本请求` → saturated=true。
- 命名遵守工程规范（无缩写、`s_` 静态前缀）；热路径保持无 Flash libm 调用（新代码只用分支比较）。
- 不 commit、不烧录（用户明确要求时才做）。

## Review Focus

- 保持窗口内 `motion_command` 可否用陈旧反馈重复入命令：`motion_feedback_fresh()` 500µs 界把陈旧反馈挡在命令入口（Task 2 测试）。
- 恢复拍 timing 检查绕过是否被滥用：仅 `s_hold_samples>0` 的单拍绕过，之后恢复 200µs 检查（Task 2 测试）。
- 抗饱和在"反馈不属于本请求"时是否误积分：不匹配即冻结，Task 3 测试以无 isr 拍验证 iq 不增长。
- 保持期间零力矩请求提交失败（epoch 失效）：按 SUBMIT 立即撤销，不得静默留在保持态（Task 2 测试）。
- 参数基准变化（Kt/方向）仍立即撤销：保持不得吞掉 PARAMETER 类（Task 2 测试）。

---

### Task 1: algo_motion 速度 PI 条件积分（饱和标志）

**Files:**
- Modify: `App/Algorithm/Inc/algo_motion.h`（`algo_motion_pi_t` 加 `bool saturated`，含 `<stdbool.h>`）
- Modify: `App/Algorithm/Src/algo_motion.c`（积分门控）
- Test: `scripts/tests/control/test_motion_math.c`

**Interfaces:**
- Produces: `algo_motion_pi_t { float integral; bool saturated; }`；`algo_motion_speed()` 签名不变。
  饱和语义：`pi->saturated=true` 时仅 `raw*error<0`（回退）允许积分。

- [x] **Step 1: 写失败测试**（`test_motion_math.c` 追加）：

```c
/* 抗饱和：外部限幅时积分冻结；误差反向仍允许回退 */
algo_motion_pi_t pi = {0};
pi.saturated = true;
float out = algo_motion_speed(&pi, 1, 0, 0, 10, .0001f, 10);
CHECK_NEAR(pi.integral, 0, 1e-9f);   /* 未积分 */
pi.integral = 1;
out = algo_motion_speed(&pi, -1, 0, 0, 10, .0001f, 10);
CHECK(pi.integral < 1);               /* 反向允许回退 */
CHECK(out < 1);                       /* out=(-1)*0+integral 已回退 */
pi.saturated = false;
(void)algo_motion_speed(&pi, 1, 0, 0, 10, .0001f, 10);
CHECK(pi.integral > 1 - 1e-6f);       /* 非饱和正常积分（回退一步后再前进） */
```

- [x] **Step 2: 跑测试确认失败**：`bash scripts/tests/control/run.sh`，期望 FAIL（saturated 字段不存在/行为不符）。
- [x] **Step 3: 实现**：`algo_motion_speed` 积分条件改为
  `unwind || (!pi->saturated && raw > -limit && raw < limit)`，其中 `unwind = (raw>0&&error<0)||(raw<0&&error>0)`。
- [x] **Step 4: 跑测试确认通过**（control 全套两配置 0 failures）。
- [x] **Step 5: Commit**（暂缓：与用户"不 commit"约定一致，统一由用户确认后提交；本计划所有任务同此）。

### Task 2: motion 坏拍有界保持

**Files:**
- Modify: `App/Control/Src/app_motion.c`（`motion_invalid` 拆为 `motion_abort`（原语义）与 `motion_reject`（active 且质量类→保持）；`s_hold_samples`/`s_velocity_gap`；恢复拍处理）
- Modify: `App/Control/Inc/app_motion.h`（snapshot 加 `uint32_t hold_count`）
- Test: `scripts/tests/control/test_motion.c`（新 `test_motion_hold()`，注册进 `test_main.c`）

**Interfaces:**
- Produces: `app_motion_snapshot_t.hold_count`（保持事件累计）；撤销语义不变（reason/清零/零请求）。

- [x] **Step 1: 写失败测试**（要点）：
  - 稳态力矩活动（mock 立回环：step→`app_foc_isr_step()`）后 `g_mock_output_failure=1` 单拍：
    `active==1`、`reason==APP_MOTION_REASON_SAMPLE`、`iq_a==0 && torque_nm==0`、`hold_count==1`、`bad_count` +1；
  - 恢复一拍：`active==1`、`iq_a>0`（回目标值）、`feedback_valid==1`，`speed_rad_s` 不被跨隙差分污染（先造非零速度，保持前后滤波值不变）；
  - 连续 6 坏拍：第 6 拍后 `active==0`、`reason==SAMPLE`、`iq_a==0`、`hold_count==2`；
  - 保持期间改 Kt 再续命令：`app_motion_torque()` 返回 -1（新鲜度/基准守卫）；活动期间改 Kt 由既有实时守卫撤销（reason==PARAMETER，立即，无保持）。
- [x] **Step 2: 跑测试确认失败**。
- [x] **Step 3: 实现**：`motion_reject(reason)` 内部判定质量类 + active → 保持路径（零力矩请求、`s_velocity_gap=true`、计数、超限 `motion_abort`）；否则 `motion_abort`（原 `motion_invalid` 全语义）。恢复拍：`s_hold_samples>0` 时跳过 timing 拒绝与测速差分，成功解算后清 `s_hold_samples`。`motion_clear_output` 清 `s_hold_samples`。
- [x] **Step 4: 跑测试确认通过**。
- [x] **Step 5: Commit**（同上暂缓）。

### Task 3: motion 消费 FOC 执行反馈 + 诊断

**Files:**
- Modify: `App/Control/Src/app_motion.c`（active 段读 `app_foc_get_execution()`，设 `s_velocity_pi.saturated` 与诊断字段）
- Modify: `App/Control/Inc/app_motion.h`（snapshot 加 `float iq_accepted_a; bool current_limited, voltage_saturated;`）
- Modify: `App/Comm/terminal/Src/app_terminal_cmd_foc.c`（`motion show` 追加 accepted iq / hold / lim / sat）
- Test: `scripts/tests/control/test_motion.c`（`test_motion_anti_windup()`）

**Interfaces:**
- Consumes: `app_foc_get_execution(app_foc_execution_t*)`（已有）。
- Produces: snapshot 新字段；`motion show` 新输出行。

- [x] **Step 1: 写失败测试**（要点）：
  - 速度模式（kp=.02 ki=10，目标 .5rad/s，转子静止）持续步进 + 每步 isr 拍：
    `i_q_max_a=10`（不钳）时 `iq_a` 随步数增长（积分生效）且 `current_limited==0`；
  - `i_q_max_a=5e-4`（钳位）时 `iq_a` 20 步基本不变（积分冻结）且 `current_limited==1`、`iq_accepted_a==5e-4`；
  - 无 isr 拍（执行反馈不属本请求）时 `iq_a` 不增长（冻结）。
- [x] **Step 2: 跑测试确认失败**。
- [x] **Step 3: 实现**：active 段（扭矩/速度公共路径）读执行反馈一次：
  `ours = valid && epoch==s_epoch && request_sequence==s_request_sequence`；
  `s_velocity_pi.saturated = !ours || current_limited || voltage_saturated || request_expired`；
  诊断字段按 ours 采信（非 ours：accepted=0、两标志 false）。
- [x] **Step 4: 跑测试确认通过**。
- [x] **Step 5: Commit**（同上暂缓）。

### Task 4: 全量回归、构建与测试流程交付

**Files:**
- Modify: `docs/superpowers/specs/2026-10-14-motion-final-review.md`（追加本轮策略与验证记录）
- Test: 全套 `scripts/tests/{encoder,foc,control,identify}/run.sh` + `make artifacts`

- [x] **Step 1: 全量回归**（-O1 与 -O2 -ffast-math 两配置全 0 failures；encoder/control 另跑 UBSan）。
- [x] **Step 2: `make artifacts`**（无 warning/error；记录 FLASH/ILM/DLM）。
- [x] **Step 3: 反汇编复核**热路径无新增 Flash 调用（`app_motion_step`/`algo_motion_speed`）。
- [x] **Step 4: 输出上板测试流程**（命令序列、限值、判据、异常处置），随最终报告交付。

## Self-Review

- 覆盖：坏拍有界保持（Task 2）、抗饱和（Task 1+3）、诊断（Task 3）、测试流程（Task 4）——与"空载力矩/速度可用"目标一一对应；位置 P 明确排除。
- 步骤粒度：每任务红→绿→验证；策略常量与判定条件均已在 Global Constraints 定死。
- 类型一致：`hold_count`/`iq_accepted_a`/`current_limited`/`voltage_saturated`/`saturated` 命名全计划一致。
- Review Focus 五项均已绑定到具体任务测试。
- 篇幅：计划与改动规模相称，无代码转录。
