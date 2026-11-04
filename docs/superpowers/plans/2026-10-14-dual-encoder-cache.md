# 双编码器25kHz同步取帧与外环缓存消费

用户已批准简化：两编码器PWM同边沿25kHz采样，ADC ISR只取帧/发布，GPTMR只编排控制外环。

## 任务与证据
- [x] 编码器宿主先复现PWM触发路由/消费者只读快照测试失败。
- [x] SPI1路由回PWM1 CH10REF；ADC取两帧、发布完整配对；游标失败不影响转子电流路径。
- [x] GPTMR恢复CH0=5kHz、CH1=10kHz纯reload中断；移除CH2触发、CH3比较及单次SYNCI扩展。
- [x] 测试配对完整性、坏帧、缓存独立发布、超龄、重复读取不取SPI、timer启动失败停止。
- [x] 测速dt改用配对快照timestamp；速度PI仍用GPTMR控制入口dt，测试不同入口延迟不改变测速。
- [x] 新增游标取帧/失败/有效配对统计与RTT生产频率观测。
- [x] 全套回归、构建、ILM/调用链检查、最终自审及测试说明；本轮未烧录。

## 范围和限制
不更改电流PI/PWM数学、转子原有有界取帧策略、保护动作开关，不提交/烧录。
没有硬件帧号；配对表示同触发源、同ADC回调两路成功取帧，仍需验证DMA/CS时序与漏拍。
配对timestamp沿用ADC入口时间，不宣称硬件CS采样时间；改善了GPTMR入口抖动影响，不消除ADC入口抖动。
SPI1零等待失败就将本拍配对无效，最后成功原码仍供调试观察；不增加ISR忙等。
不替整个配置文件；不修改已有Flash标定布局，不需要重新标定相位/零位。
不使用额外实施/评审子代理，Mephisto亲自执行与复核。

## 最终验证记录
encoder165/foc289/control452159/identify77 checks，-O1及-O2 -ffast-math均0失败。
make artifacts成功，无warning/error；工作区diff及HEAD总diff空白检查通过。
FLASH206680B、ILM20496B、DLM117696/130304B（90.32%）。
nm：app_encoder_output_sample=0x2950、app_encoder_sync_sample=0x29e6、app_motion_step=0x47c8，均ILM。
反汇编：ADC取帧函数只有两路缓存fetch间接调用及快照/计时调用；没有游标解算或外环PI。
Interface及drv_trgm相对HEAD无改动，已撤净本轮GPTMR独立SPI触发扩展。
未提交、未烧录、未重新暂存；原index与最新工作区不同，提交前需重审并重新暂存。
