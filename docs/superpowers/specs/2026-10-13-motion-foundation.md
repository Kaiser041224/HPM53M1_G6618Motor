# Motion平台阶段一、二交付说明

> 当前双编码器采样实现见 `2026-10-14-dual-encoder-cache.md`。下文为早期阶段记录。

## 已实现
- GPTMR1 CH0=5kHz位置观测、CH1=10kHz速度观测，同实例共享IRQ、驱动按通道顺序分发。
- GPTMR1 PLIC优先级2，ADC0保持3；DISABLE_IRQ_PREEMPTIVE=1不变，不支持ADC抢占已运行motion ISR。
- app_motion_timer只读编码器一致快照，记录周期范围、最大回调耗时、超过标称周期10%的late次数、反馈年龄。
- Application在转子同步采样启动尝试之后启动观测；无论FOC是否使能均有观测。无motion控制输出。
- app_gptmr注册回调保持频率，NULL注销；TIMER允许NULL但不启用该通道IRQ；set_frequency原驱动启动语义保留并明确注明。
- 统一MOTION电流请求：原子id/iq、sequence、epoch；DIRECT保持旧终端电流接口。
- source仅OFF/零给定READY切换。使能、关闭、校准开始/结束、软停止撤销旧epoch。
- MOTION不能调用legacy电流setter或切强制角；CALIB专用端口不受影响。
- ADC消费MOTION请求；后台只在DIRECT/STOPPING复制旧cmd，不覆盖motion输出。
- 请求500us租约：超龄零电流给定回退，不关桥、不锁FAULT；每次成功提交以本机mcycle续租。
- 执行反馈区分requested/accepted/measured；valid依据本拍电流执行返回值；累计invalid/limited/saturated供消费者差分。

## 数据契约
任务/GPTMR调用app_foc_submit_current前需选择MOTION并获取当前epoch。
正常每100us发布，sequence由唯一生产者维护；FOC记录但不校验sequence单调性。
请求没有通讯超时含义，500us仅是内部控制生产者租约。反馈valid代表正常电流路径本拍执行成功，
不代表请求未限幅；request_expired单独标注。CALIB/VTEST/STOPPING时normal_owner/valid=false。
所有结构读写均短临界区；ADC不会自旋等待任务；不同ISR未开启嵌套。

## 未实现/未验证边界
- 未实现力矩换算、速度PI、位置P，也未开放motion终端命令。
- 双计数器顺序start，未硬件同步启动；不能承诺严格位置先于每第二个速度拍。
  共享待处理中断按CH0→CH1消费。正式位置控制前需确定相位同步启动/锁存策略。
- 观测周期是回调入口间隔，execution是回调内部耗时，不含FreeRTOS/PLIC完整ISR开销。
- 未实测GPTMR触发延迟、实际频率、丢事件、ADC入口延迟、USB负载、任务/ISR栈高水位。
- PLIC优先级只改变pending仲裁，GPTMR已经执行时仍阻塞ADC。
- UART/USB等其他ISR也可能阻塞电流路径，优先级不能代替端到端截止时间验证。
- DIRECT legacy id/iq分别setter仍是调试兼容路径；motion输入已完整原子化。
- 不拆采样与执行入口、不改电流PI/调制数学、不新增全局保护动作。
- 不解决先前电流测量偏置及1kHz动态诊断不通过问题。

## 验证
2026-10-13宿主两遍（-O1/-O2 -ffast-math）：encoder127、foc289、control452009、identify77 checks，0失败。
make artifacts成功，无warning/error；git diff --check通过。
FLASH202896B、ILM17752B、DLM117664/130304B。
nm确认gptmr分发、motion观测、编码器一致读取、请求提交、irq save/restore在ILM；
ISR包装保留SDK .isr_vector属性，未强行叠加.fast属性。

## 上板验证
外部供电；先FOC OFF读取RTT [MOTION]日志。480MHz时标称period：position96000、velocity48000 cycles。
count应约5000/10000每秒增长，反馈seq增长；max/late需同时观察ADC ISR计数与间隔。
然后低电流验证foc on/iq/off、cal encoder/cal rl及取消。USB输入/输出压力下重新测延迟。
未证实40us电流截止时间前，不启用motion非零输出。

## 回滚
本次未提交/烧录；用git diff逐文件回退本次修改，不使用git reset --hard。
CMake原文件额外备份：/tmp/opencode/CMakeLists.txt.before-motion-foundation-20261013。
