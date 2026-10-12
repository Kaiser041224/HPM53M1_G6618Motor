# Rs/Ld 与电流 PI 自整定 Implementation Plan

**Goal:** 正式隔离模块辨识等效 Rs、Ld，计算并验收共用 d/q PI；显式 apply/save。
**Architecture:** 1kHz app_io 状态机；电流端口有界同步采集；纯算法任务上下文运行。
**Tech Stack:** C17、FreeRTOS、现有 FOC；无新依赖。
**Spec:** 本会话批准设计，直接实现，不新增保护关机策略。

## Constraints / Review Focus
- 不改变 ADC/PWM/编码器触发链，不新增任务，不改变 foc_current_step 数学。
- 原有保护不变；有限激励、数据校验、互斥、取消恢复仍保留。
- 自由转子，已应用编码器零点；正d偏置；Lq=Ld近似。
- 平台1/2/3/2/1A，400ms后捕获128拍；2A附近正负0.1V脉冲。
- 临时PI不改正式参数，200/300/500Hz阶跃验收，完成不自动apply。
- 前一拍指令电压与本拍电流配对；实际PWM延迟需上板验证。
- 非有限/退化数据、取消、FOC OFF、重入、持久化匹配、ISR发布完整性必须测试。
- 不覆盖配置，不自动提交。

## Tasks
- [x] 纯算法：RED → Rs/Ld拟合、PI与阶跃验收 → GREEN。
- [x] 同步端口：RED → 捕获/有限脉冲/临时PI/取消 → GREEN。
- [x] 正式模块：RED → 状态机/结果/apply/save/restore → GREEN。
- [x] Terminal/CMake/app_io/boot接入，保持电角度功能。
- [x] 全宿主回归、构建、diff、自审；列出未验证硬件事项。

验证：encoder127/foc289/control101752/identify77 checks，均-O1与-O2 -ffast-math两遍通过。
control大量check是对象仿真逐拍返回值检查，不代表101752个独立场景。
make artifacts退出0，无warning/error；FLASH193640B，ILM15576B。未上板。
