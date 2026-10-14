# Motion平台阶段一、二实施计划

**Goal:** GPTMR1 CH0位置5kHz、CH1速度10kHz观测平台与FOC统一电流请求，为motion提供接口，不启用外环控制。
**Architecture:** Application连接GPTMR平台观测；ADC继续唯一采样与电流执行。FOC正常请求与校准/停止路径隔离，使用权限代次撤销旧请求。
**Constraints:** C17；不改电流PI/调制数学；非向量、非抢占不变；不自动提交/烧录；硬件时序需实测。

## 检查项
- [x] Task 1: GPTMR配置保存、注册不复位频率、motion双通道观测；宿主平台测试RED→GREEN。共享IRQ顺序源码复核，非硬件实测。
- [x] Task 2: `app_foc_current_request_t`原子id/iq、source/epoch/sequence；坏输入、校准/停止、旧epoch、租约测试RED→GREEN。
- [x] Task 3: ADC执行反馈及累计统计；motion只读编码器快照；初始化最后启动观测。
- [x] Task 4: 全套两遍回归、无警告构建、diff检查、ILM符号检查、自审与上板验证说明。硬件验证未执行。

## Review focus
注册回调覆盖频率；GPTMR共享IRQ先位置后速度；后台覆盖ISR请求；旧请求在校准/重启恢复；反馈不得将accepted当作measured。

## 执行记录与裁决
- 使用当前干净feature分支工作区，避免改变构建路径；不创建额外worktree、不提交。
- 不拆ADC采样/执行函数：GPTMR不需要同拍Application连接，保留现有ADC入口，降低采样链回归风险。
- 首版采用权限显式选择DIRECT/MOTION，只允许OFF或零给定READY切换；legacy setter只在DIRECT正常模式有效。
- motion定时器只运行观测，不发布任何非零或零电流请求；原控制路径保持可用。
- 定时器同步启动/触发延迟需硬件证据，首版仅报告入口周期与执行耗时，不伪称锁相或测得硬件触发延迟。
- 内部MOTION请求500us租约，超龄零给定而非停桥，避免生产者中止后无限持有旧输出；不是CAN超时策略。
- 自审发现并修复：MOTION首次非零提交推进RUN；撤销清payload避免诊断残留；反馈valid必须本拍确实执行；timer重启清统计。
- 专业判断/评审由Mephisto亲自执行，未派发review子代理。
