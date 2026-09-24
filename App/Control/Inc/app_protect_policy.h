/**
 * @file    app_protect_policy.h
 * @brief   保护动作策略开关（M1：保护项只判断、不执行动作）
 * @author  Kaiser
 *
 * M1 裁决（Kaiser 2026-09-23）：保护项（过流/OV-UV/编码器/ADC/ISR 预算等）
 * 当前阶段**只做检测、计数与告警，不执行任何停机类动作**（不关桥、不停快路径、
 * 不锁存 FAULT、不拒绝指令）；外部保护 = 可调电源限流。
 *
 * 保留不动作的两类（非保护项）：
 *   - 输入无效拍的零矢量回退（控制路径输入校验，防止 NaN/垃圾占空比入桥）；
 *   - 手动/系统安全：Ozone estop 邮箱、app_rtos_fatal、异常指纹。
 *
 * 后续把保护动作逐项加回时置 1（或改为按位开放），动作语义即恢复
 * （含 emergency 关桥、快路径停用、FAULT 锁存）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_PROTECT_POLICY_H
#define APP_PROTECT_POLICY_H

/** 保护动作总开关：0 = 只判断不动作（M1）；1 = 执行停机类动作 */
#define APP_PROTECT_ACTION_EN (0)

#endif /* APP_PROTECT_POLICY_H */
