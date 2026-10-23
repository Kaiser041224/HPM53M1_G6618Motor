# P0-A/P0-B Electrical Angle + Speed/ωe Observer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement P0-A FOC angle output-delay compensation and P0-B a single 30kHz rotor speed/ωe observer; FOC and the speed loop share the observer's ωe/speed, while the position loop keeps the full rotor+vernier solve.

**Architecture:** A pure `algo_speed_observer` type-2 PLL runs in the ADC0 PMT ISR on the raw rotor angle at 30kHz, publishing `omega_e` and output-shaft speed to FOC and the 15kHz motion loop. `foc_angle` keeps raw θe + offset and gains a pure `foc_angle_advance()` helper; `app_foc_current_run_fresh()` uses the advanced angle only for inverse Park. Speed/torque modes use rotor/observer feedback; position mode keeps the full vernier pair.

**Tech Stack:** C17, HPM5361 SDK, CMake/Make, host C test harness (`cc`, `scripts/tests/*`), YAML parameter generation.

**Spec:** `docs/superpowers/specs/2026-10-18-control-loop-odrive-optimization-design.md`

## Global Constraints

- 30kHz current loop execution, 2kHz current-loop bandwidth; 15kHz speed loop; 10kHz position loop. Do not change these rates.
- Three-phase current sampling, 8MHz encoder SPI, 50:1 reducer, single-turn output; no new hardware. P1/P2 (inertia feedforward, limits, input trajectory, current-loop details) are out of scope for this plan.
- Observer state/update at 30kHz; speed published to the 15kHz loop. Observer output is mechanical; FOC conversion is `ωe = pole_pairs × direction × ωm`; speed is `ωm / reducer`.
- FOC angle: Park uses raw θe + offset; inverse Park uses `foc_angle_advance(theta_e, omega_e, output_delay_s)`. Never feed the observer position into Park.
- FOC consumers of ωe: decoupling (default off), `speed_max_rad_s` over-speed protection (default 0), telemetry. Speed loop consumes `s_motion.speed_rad_s`.
- Speed/torque modes may use rotor-only feedback; position mode must use the complete rotor+vernier pair and `algo_motion_vernier`.
- `foc_angle_advance` and the observer step are hot-path functions: `FOC_ATTR_RAMFUNC`, no Flash calls, no RTOS, no printing, C17.
- Hold/fallback at 15kHz: `APP_MOTION_HOLD_MAX_SAMPLES=300` (20ms), `APP_MOTION_VERNIER_FALLBACK_MAX_SAMPLES=7500` (500ms).
- Observer and output-delay parameters live in `config/software.yaml` software domain; flash migration must not silently discard saved motion/pos/friction tuning.
- 100rpm output is the intentional upper test point (5000rpm motor, ~833Hz electrical); do not change it to 60rpm.
- Do not commit, push, or flash unless Kaiser explicitly asks. Each task ends with a diff report, not a commit.

## Review Focus

- **1rpm output speed ripple:** observer low-speed quantization/PLL bandwidth; must be measured via the debug globals, not `foc trace`.
- **100rpm/833Hz FOC angle:** output-delay compensation must keep Park/inverse Park phase error bounded; current ripple must not increase.
- **Reversal/backlash:** observer must not overshoot or reverse-lock at direction changes; full rebound test.
- **Encoder bad frame/hold:** observer and angle chain must degrade safely; no stale angle to Park, no speed loop runaway.
- **Observer fault coupling:** P0-B fault affects FOC decoupling/limit and speed loop; `foc_angle` fallback must still work.
- **Params migration:** extending the software record must preserve saved motion/pos/friction tuning.
- **Rotor-only vs position:** speed/torque must not accidentally use the position pair; position must not use rotor-only degradation.
- **ISR budget:** observer + angle advance must stay within the 30kHz budget; no Flash calls in the hot path.

---

## File Structure

- Create: `App/Algorithm/Inc/algo_speed_observer.h`
- Create: `App/Algorithm/Src/algo_speed_observer.c`
- Create: `scripts/tests/control/test_algo_speed_observer.c`
- Modify: `App/Algorithm/FOC/Inc/foc_angle.h`
- Modify: `App/Algorithm/FOC/Src/foc_angle.c`
- Modify: `scripts/tests/foc/test_foc_angle.c`
- Modify: `App/Control/Inc/app_foc_current.h`
- Modify: `App/Control/Src/app_foc_current.c`
- Modify: `App/Control/Src/app_foc.c`
- Modify: `App/Control/Src/app_motion.c`
- Modify: `App/Platform/Inc/app_software_params.h`
- Modify: `config/software.yaml`
- Modify: `scripts/tests/fixtures/software.yaml`
- Modify: `scripts/tests/control/run.sh`
- Modify: `scripts/tests/control/test_main.c`
- Modify: `scripts/tests/control/test_motion.c`
- Modify: `scripts/tests/control/test_foc_current_integration.c`
- Modify: `scripts/tests/params/test_main.c`
- Modify: `CMakeLists.txt`

---

### Task 1: `foc_angle_advance` (P0-A angle output delay)

**Files:**
- Modify: `App/Algorithm/FOC/Inc/foc_angle.h`
- Modify: `App/Algorithm/FOC/Src/foc_angle.c`
- Test: `scripts/tests/foc/test_foc_angle.c`

**Interfaces:**
- Produces: `float foc_angle_advance(float theta_e_rad, float omega_e_rad_s, float delay_s);`
- Returns `foc_wrap_2pi(theta_e_rad + omega_e_rad_s * delay_s)`.
- Non-finite `theta_e_rad` returns `0.0f`; non-finite `omega_e_rad_s`/`delay_s`, or `delay_s <= 0`, returns `foc_wrap_2pi(theta_e_rad)`.

- [ ] **Step 1: Add failing checks to `test_foc_angle()`**

```c
/* output delay: theta=0, 100 rad/s, 1/30k -> 0.0033333334 */
CHECK_NEAR(foc_angle_advance(0.0f, 100.0f, 1.0f / 30000.0f), 0.0033333334f, 1e-6f);
/* 100rpm output = 5236 rad/s electrical; 1/30k -> 0.174533 rad (10 deg) */
CHECK_NEAR(foc_angle_advance(0.0f, 5236.0f, 1.0f / 30000.0f), 0.174533f, 1e-5f);
/* wrap: 6.2 + 1.0 = 7.2 -> 0.9168147 */
CHECK_NEAR(foc_angle_advance(6.2f, 100.0f, 0.01f), 0.9168147f, 1e-5f);
/* negative speed: 1.0 - 1.0 = 0 */
CHECK_NEAR(foc_angle_advance(1.0f, -100.0f, 0.01f), 0.0f, 1e-6f);
/* invalid omega/delay/theta: safe hold */
CHECK_NEAR(foc_angle_advance(1.0f, NAN, 0.01f), 1.0f, 1e-6f);
CHECK_NEAR(foc_angle_advance(1.0f, 100.0f, NAN), 1.0f, 1e-6f);
CHECK_NEAR(foc_angle_advance(1.0f, 100.0f, 0.0f), 1.0f, 1e-6f);
CHECK_NEAR(foc_angle_advance(NAN, 100.0f, 0.01f), 0.0f, 1e-6f);
```

- [ ] **Step 2: Run and verify failure**

Run: `bash scripts/tests/foc/run.sh`
Expected: compile/link failure because `foc_angle_advance` is not declared/defined.

- [ ] **Step 3: Declare and implement `foc_angle_advance`**

Add the prototype to `foc_angle.h` near the step API, with `FOC_ATTR_RAMFUNC` on the definition in `foc_angle.c`. Use `foc_finite`, `foc_wrap_2pi`; no new state.

- [ ] **Step 4: Run and verify pass**

Run: `bash scripts/tests/foc/run.sh`
Expected: both `-O1` and `-O2 -ffast-math` passes; all checks pass.

- [ ] **Step 5: Report diff; do not commit**

Run: `git diff --stat -- App/Algorithm/FOC scripts/tests/foc`
Report the files and test counts. No commit.

---

### Task 2: `algo_speed_observer` (P0-B core)

**Files:**
- Create: `App/Algorithm/Inc/algo_speed_observer.h`
- Create: `App/Algorithm/Src/algo_speed_observer.c`
- Create: `scripts/tests/control/test_algo_speed_observer.c`
- Modify: `scripts/tests/control/test_main.c`
- Modify: `scripts/tests/control/run.sh`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `typedef struct { float bandwidth_rad_s; float sample_time_s; float max_speed_rad_s; } algo_speed_observer_cfg_t;`
- `int algo_speed_observer_init(algo_speed_observer_t* self, const algo_speed_observer_cfg_t* cfg);`
- `float algo_speed_observer_step(algo_speed_observer_t* self, float theta_meas_rad, float dt_s, float* omega_rad_s_out);`
- `void algo_speed_observer_reset(algo_speed_observer_t* self);`
- `float algo_speed_observer_get_omega(const algo_speed_observer_t* self);`
- `float algo_speed_observer_get_theta(const algo_speed_observer_t* self);`
- Gains: `kp = 2·bandwidth_rad_s`, `ki = bandwidth_rad_s²`.
- First valid sample primes `theta = wrap_2pi(theta_meas)`, `omega = 0`; invalid `dt` (`<=0` or `>5ms`) or non-finite `theta_meas`/`dt` holds `omega`; `omega` clamps to `±max_speed_rad_s`; optional `snap_to_zero` when `|omega| < 0.5·dt·ki`.

- [ ] **Step 1: Write `test_algo_speed_observer.c` and wire it**

Add `test_algo_speed_observer()` declaration/call to `test_main.c`, and `App/Algorithm/Src/algo_speed_observer.c` to `APP_SRC` in `scripts/tests/control/run.sh`. Add the source to `CMakeLists.txt` with `sdk_app_src(App/Algorithm/Src/algo_speed_observer.c)`.

Test cases:
```c
/* first sample */
cfg = {500.0f, 1.0f/30000.0f, 700.0f};
step(0.1f, ts, &w) -> w == 0; get_theta ≈ 0.1;
/* constant mechanical speed 10 rad/s, 2000 steps */
-> get_omega ≈ 10.0 ± 0.5;
/* 1rpm output at 50:1 = 5.236 rad/s mechanical, 2000 steps -> ≈5.236 ± 0.1 */
-> get_omega ≈ 5.236 ± 0.1;
/* reverse speed -10 rad/s -> ≈ -10 ± 0.5 */
/* 0/2π wrap and 3x dt jitter -> omega finite and bounded */
/* NaN theta / dt=0 / dt=1e-2 -> omega holds; no NaN */
/* |omega| > max -> clamps to ±700 */
/* reset -> next step omega 0 */
```

- [ ] **Step 2: Run and verify failure**

Run: `bash scripts/tests/control/run.sh`
Expected: compile failure because the header/source/test symbol does not exist.

- [ ] **Step 3: Implement the pure observer**

Implement the type-2 PLL with no `sinf/cosf/atan2f`; use `foc_wrap_2pi`, `foc_wrap_pm_pi`, `foc_finite`; `FOC_ATTR_RAMFUNC` on `step`/getters. The object uses function pointers + explicit state, matching `algo_motion` style.

- [ ] **Step 4: Run and verify pass**

Run: `bash scripts/tests/control/run.sh`
Expected: both passes; observer checks pass.

- [ ] **Step 5: Report diff; do not commit**

Run: `git diff --stat -- App/Algorithm scripts/tests/control CMakeLists.txt`
No commit.

---

### Task 3: FOC integration — observer ωe + angle advance

**Files:**
- Modify: `App/Control/Inc/app_foc_current.h`
- Modify: `App/Control/Src/app_foc_current.c`
- Modify: `App/Control/Src/app_foc.c`
- Modify: `scripts/tests/control/mock_foc_deps.c` (add `g_mock_rotor_fail` for the encoder-failure path)
- Test: `scripts/tests/control/test_foc_current_integration.c`
- Test: `scripts/tests/control/test_main.c` (if a new test entry is added)

**Interfaces:**
- `app_foc_current_snapshot_t` gains `float speed_out_rad_s;`.
- `app_foc_current_run_fresh(...)` uses `foc_angle_advance(theta_e_rad, omega_e_rad_s, output_delay_s)` for the inverse Park only; Park keeps `theta_e_rad`.
- `app_foc` owns `algo_speed_observer_t`; steps it from `enc.rad` and the ISR `dt`; publishes `omega_e`/`speed_out` into the snapshot and uses the observer ωe for `speed_max` and the current loop. Fallback to `foc_angle` ωe when the observer is not valid.
- `output_delay_s` default: `1.0f / 30000.0f` until Task 5 wires the YAML parameter.

- [ ] **Step 1: Add failing integration checks**

In `test_foc_current_integration.c`, add a test that runs `app_foc_current_run_fresh()` with `theta_e=0`, `iq_ref=1`, feedback `iq=0`, `omega_e=0` and `omega_e=100`; assert the captured duty arrays differ and both stay within `duty_max`. Reset the current controller between runs so the PI state is identical.
Add a `test_foc_speed_observer()` (control suite) that:
- `app_foc_init()`, `app_foc_enable()`;
- steps `g_mock_rotor_rad` at a constant rate and calls `app_foc_isr_step()` N times;
- reads `app_foc_get_snapshot()`;
- checks `snapshot.omega_e_rad_s ≈ pole_pairs × direction × omega_m` and `snapshot.speed_out_rad_s ≈ omega_m / reducer`.
- 833Hz case: `omega_e=5236`, `theta_e=0`, `output_delay=1/30000`; the inverse-Park duty differs from `omega_e=0` and stays inside the duty window.
- Encoder-bad case: set `g_mock_rotor_rad=NAN` for one frame -> existing bounded hold; set `g_mock_rotor_fail=1` for more than the bad-frame threshold -> assert `g_foc_protect_counts[APP_FOC_PROT_ENC]` increments and the existing M1 safe path (held/zero per `APP_PROTECT_ACTION_EN`) is finite; no NaN in duty.

- [ ] **Step 2: Run and verify failure**

Run: `bash scripts/tests/control/run.sh`
Expected: fail because `speed_out_rad_s`/observer/angle-advance integration is absent.

- [ ] **Step 3: Implement FOC integration**

- Add `speed_out_rad_s` to the snapshot.
- Step the observer in `app_foc_isr_step()`; on encoder bad/held samples, invalidate the observer speed and keep the existing bounded `s_theta_held` behavior for short dropouts; after the configured bad-frame threshold, keep the existing M1 safe path (held/zero per `APP_PROTECT_ACTION_EN`; count `APP_FOC_PROT_ENC`), do not change the M1 action; never use a stale angle indefinitely without counting.
- Convert `omega_m` to `omega_e`/`speed_out` using motor params (`pole_pairs`, `encoder.direction`, `reducer_ratio`).
- Use `foc_angle_advance()` for the inverse Park in `app_foc_current_run_fresh()`; keep `theta_e` for Park.
- Use the observer ωe in `app_foc_run_body()` `s_speed_limited`, `foc_current` decoupling, and `g_foc_current_snapshot.omega_e_rad_s`; fallback to `foc_angle` ωe when observer invalid.

- [ ] **Step 4: Run and verify pass**

Run: `bash scripts/tests/control/run.sh`
Expected: both passes; observer/speed/angle checks pass.

- [ ] **Step 5: Report diff; do not commit**

No commit.

---

### Task 4: Speed loop integration — observer speed, rotor-only, hold/fallback

**Files:**
- Modify: `App/Control/Src/app_motion.c`
- Modify: `scripts/tests/control/test_motion.c`
- Modify: `scripts/tests/control/test_main.c` (only if new test entries)
- Test: `scripts/tests/control/test_motion.c`

**Interfaces:**
- `app_motion_step()` reads `app_foc_get_snapshot().speed_out_rad_s` and uses it for `s_motion.speed_rad_s` when the observer is valid; fallback to the existing differential/LPF if invalid/disabled.
- Speed/torque modes use rotor/observer feedback only; position mode keeps `app_encoder_output_sample()` + `algo_motion_vernier` + `shaft_rad`.
- `APP_MOTION_HOLD_MAX_SAMPLES=300`; `APP_MOTION_VERNIER_FALLBACK_MAX_SAMPLES=7500`.
- `s_motion.speed_rad_s` is output-shaft mechanical rad/s; no extra LPF when the observer is active (`velocity_lpf_coeff=1.0`), LPF remains for fallback.

- [ ] **Step 1: Update failing motion tests**

- `test_motion_params`: replace the differential-speed assertion with a helper that runs the observer for N steps at constant rotor speed; expect `snapshot.speed_rad_s` within 2% of the expected output speed.
- `test_motion_sample_time`: drive the observer; the measured speed must not depend on entry jitter.
- `test_motion_hold`/`test_motion_vernier_fallback`: update expected sample counts to 300/7500.
- Add a speed/torque test: `g_mock_output_failure=1` (pair invalid) must not zero the speed/torque loop when the rotor/observer is valid; position mode must still hold.
Run: `bash scripts/tests/control/run.sh`; expect failures.

- [ ] **Step 2: Run and verify failure**

Run: `bash scripts/tests/control/run.sh`
Expected: fail on the old differential expectations / missing observer speed / old hold counts.

- [ ] **Step 3: Implement `app_motion` integration**

- Read the FOC snapshot at the start of `app_motion_step()`; use `speed_out_rad_s` when `valid`.
- Branch by mode: position uses the pair/vernier; speed/torque uses rotor/observer; calibrating keeps the pair.
- Update hold/fallback constants to 300/7500.
- Keep `s_motion.speed_rad_s` output-side; no double LPF with the observer.
- Preserve `s_motion.feedback_valid`, `motion_zero`, `calibrate`, `apply`, `save` semantics.

- [ ] **Step 4: Run and verify pass**

Run: `bash scripts/tests/control/run.sh`
Expected: both passes.

- [ ] **Step 5: Report diff; do not commit**

No commit.

---

### Task 5: `software.yaml` parameters + flash migration

**Files:**
- Modify: `config/software.yaml`
- Modify: `App/Platform/Inc/app_software_params.h`
- Modify: `scripts/tests/fixtures/software.yaml`
- Modify: `scripts/tests/params/test_main.c`
- Modify: `App/Control/Src/app_foc.c` / `app_motion.c` (consume parameters)

**Interfaces:**
- `control.speed_observer.enable` (default 1), `bandwidth_rad_s` (default 500.0), `max_speed_rad_s` (default 700.0).
- `control.current_loop.output_delay_s` (default `3.3333333e-5`, one 30kHz period).
- `control.limits.motion_iq_max_a` and `control.limits.i_q_max_a` aligned (recommended 24.3A; document if 25A remains).
- Flash migration: extend the software record with a versioned/additive layout; do not silently reset saved motion/pos/friction tuning. If a generation bump is unavoidable, document the one-time reset and test it.

- [ ] **Step 1: Add failing params tests**

Add YAML/struct checks for the new fields and the `motion_iq_max`/`i_q_max` alignment; add a flash round-trip test that preserves a non-default motion gain.
Run: `bash scripts/tests/params/run.sh`; expect failure.

- [ ] **Step 2: Run and verify failure**

Run: `bash scripts/tests/params/run.sh`
Expected: fail on missing fields/migration.

- [ ] **Step 3: Implement YAML/struct/migration + consumers**

Wire `app_foc` observer bandwidth/enable, `app_foc_current` output delay, `app_motion` observer enable/fallback. Update `generation`/record version as needed and implement the additive migration.

- [ ] **Step 4: Run and verify pass**

Run: `bash scripts/tests/params/run.sh && bash scripts/tests/control/run.sh`
Expected: pass.

- [ ] **Step 5: Report diff; do not commit**

No commit.

---

### Task 6: Debug globals, hygiene, full validation

**Files:**
- Modify: `App/Control/Src/app_motion.c`
- Modify: `App/Debug/Src/app_debug_motor.c` (or the existing motor debug hook)
- Modify: `App/Platform/Src/app_analog_signal.c` (filter sample rate: process is 1kHz, not pwm_freq; current filters NONE, latent fix)
- Modify: `scripts/tests/control/test_motion.c`
- Modify: comments/constants in `app_motion.h`, `app_foc.h`, `app_foc_current.h`, `app_motion_timer.h`, `app_encoder.h`, `app_analog_signal.h`, `app_logic.c`
- Modify: `APP_FOC_ENC_PERIOD_US`, `APP_FOC_ISR_BUDGET_US`, `APP_FOC_CURRENT_AVG_ALPHA`, `APP_FOC_TRACE_MAX` comments/values as decided

**Interfaces:**
- Debug globals (`.noncacheable.bss`, debug only): `g_motion_speed_dbg_rad_s`, `g_motion_speed_ripple_pp_dbg`, `g_motion_speed_ripple_max_dbg`; 1kHz window or speed-tick update; Ozone/RTT readable.
- Stale-comment cleanup only where it changes no behavior; functional constants are changed deliberately and tested.

- [ ] **Step 1: Add debug-global checks**

Add a test that steps the motion loop at a constant/rippled speed and checks the debug globals are updated/finite; expect failure.

- [ ] **Step 2: Run and verify failure**

Run: `bash scripts/tests/control/run.sh`
Expected: fail on missing globals.

- [ ] **Step 3: Implement debug globals + hygiene**

Add the globals and 1kHz window; expose via the existing debug/Ozone path. Update the hold/fallback comments, `motion_iq_max` comment, 25k/5k comments, `app_encoder_output_start()` age constant, and the FOC trace/average comments/values as specified. Fix `app_analog_signal`'s LPF `sample_rate_hz` to the 1kHz process rate (or guard it); current filters are NONE, so this is a latent correction; add a comment.

- [ ] **Step 4: Full validation**

Run:
```bash
bash scripts/tests/encoder/run.sh
bash scripts/tests/foc/run.sh
bash scripts/tests/control/run.sh
bash scripts/tests/identify/run.sh
bash scripts/tests/params/run.sh
make configure
make artifacts
git diff --check
```
Expected: all pass; build success with no warnings; diff check clean.
Hot-path check: disassemble `algo_speed_observer_step`, `foc_angle_advance`, `app_motion_step`, `app_foc_isr_step`, `foc_current_step`; no `jal/jalr` into Flash. Report ISR cycles before/after.

- [ ] **Step 5: Bench checklist (manual, not automated)**

- 1/10/100rpm output: speed ripple via debug globals; 100rpm ~833Hz electrical FOC angle/current.
- Reversal/backlash; encoder bad-frame hold; observer fault/fallback.
- `cal encoder`/`cal rl` still pass; `foc trace` current ripple not increased.
- No commit/push/flash; report results.

---

## Self-Review Notes

- Spec coverage: P0-A output delay (Tasks 1/3), P0-B observer (Tasks 2/3/4), FOC/speed sharing (Tasks 3/4), rotor-only/position (Task 4), params (Task 5), debug/hygiene (Task 6).
- Inter-sample interpolation is deferred: the encoder and current loop are both 30kHz; the 10°/sample at 833Hz is the sample interval, not a missing sample between the encoder and current. Output delay is the implemented angle correction; the bench checklist must confirm at 833Hz whether more interpolation is needed.
- 1rpm/833Hz/encoder-bad cases are covered in Tasks 2/3; the `app_analog_signal` latent sample-rate issue is in Task 6.
- Types: `foc_angle_advance` returns float; observer step returns mechanical omega; snapshot `speed_out_rad_s` is output-shaft rad/s; `omega_e` is electrical rad/s.
- Review Focus: each item has an automated test or a named bench check.
- No commit/push/flash; native execution is the only feasible implementation path in this environment because the general subagent is denied.
