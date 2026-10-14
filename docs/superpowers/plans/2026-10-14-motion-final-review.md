# Motion最终提交前复核与局部优化

基线c94e206。用户要求review/优化/准备提交，保留PWM双编码器25kHz + GPTMR外环10kHz/5kHz。

- [x] 请求所有权：MOTION拒绝vtest、强制角/未消费非零请求时不得切source。
- [x] 500us租约过期锁存，32bit时间回绕不得复活旧请求。
- [x] motion活动命令拒绝Kt/方向变化；显式中止原因与零输出观测；数据非法不入控制。
- [x] 定时观测按报告窗口清指标；配对统计原子读取，禁止计数撕裂。
- [x] 参数持久化边界、数学异常、头文件/热路径、全量回归与最终diff自审。
- [x] 交付风险/上板回归说明；重新暂存并提供具体commit message，不commit/push。

范围：不自动恢复坏帧后的motion输出，不改非抢占，不改PI/采样架构，不新增保护停桥。
专业判断/代码修改/评审由Mephisto亲自执行；不派发评审代理。

## 验证记录
2026-10-14：encoder168/foc289/control452218/identify77 checks，-O1及-O2 -ffast-math均0失败。
encoder/control额外UBSan及float-cast-overflow、两配置均通过。
make artifacts无warning/error，FLASH206944B、ILM20752B、DLM117696B（90.32%）。
HEAD diff空白检查通过；反汇编motion/PI/vernier/delta无Flash调用；取帧/配对消费/控制函数均ILM。
未烧录。本轮自审不等于独立评审；新增输出拒绝与参数恢复路径仍需上板回归。
