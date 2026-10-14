# Motion力矩/速度开发版测试

> 采样链与测速时间基准已更新，参见 `2026-10-14-dual-encoder-cache.md`；本文件保留早期实现与命令测试步骤。

## 状态
未烧录/未提交。默认未使能，速度增益0。固定49/50游标及50减速，参数化齿数后续完成。
SPI1采用PWM1 CH10REF→DMA_REQ1→HDMA1启动，GPTMR10kHz取最新帧，无阻塞SPI。
两SPI同触发但没有硬件帧号配对，ADC入口与GPTMR不同顺序可能错一拍；必须实测，不能声称严格同拍。
读取失败、游标残差、异常周期取消motion活动并零请求，不自动恢复。不新增关桥策略。
转矩指轴端模型Nm，tau/(50*Kt)换算，效率未辨识，固定1A请求上限，速度命令±0.5轴端rad/s。
位置环未实现，5kHz仍观测。速度PI未接内环执行摘要抗饱和，仅自身输出条件积分。

## 安装相位
OFF静止1000对样本，约0.1s；运动/相位不稳拒绝。相位取mod1/50，选择圈编号约定。
motion cal仅生成试用结果；motion apply应用并使旧机械零点失效；motion zero另设当前轴端零位。
motion save显式保存MOTN记录，方向变化拒绝恢复。增益仅RAM，重启为0。
原始单圈解算不代表输出轴掉电多圈位置。机械拆装后重新cal/apply/zero/save。

## 首轮测试（外部供电，先不加负载）
1. foc off，等待OFF；motion show；确认无自动输出。
2. 保持轴静止：motion cal；等待0.2s；motion show，phase trial done=1。
3. motion apply；等待新反馈；motion show应cal=1 valid=1。
4. 手动慢转，检查shaft连续跨0/2pi、实际转子50圈对应轴一圈、bad计数/速度噪声。
   bad持续增加、7.2deg跳变或周期异常：停止，不进入闭环。
5. 回到机械基准：motion zero；motion save；重启检查恢复。
6. foc on；motion torque 0.05（名义iq≈0.00855A，非常小，未必克服摩擦）；motion show。
   实际响应不足也不要立即大幅加给定，先检查方向/电流/噪声。
7. motion off；等待OFF；motion direct可恢复旧foc电流调试。
8. 速度试验：OFF配置motion gains 0.05 0（Nm/(rad/s)、Nm/rad），foc on；motion speed 0.05。
   该增益仅低增益试验建议，不是整定结果；先检查P，再小步加I，记录母线与电流。
9. 每步都观察RTT [MOTION]回调max、ADC最大间隔、USB负载，40us截止时间未证实前不得升性能。

## 证据
宿主-O1/-O2 -ffast-math：encoder127/foc289/control452128/identify77 checks，0失败。
构建无warning/error，FLASH206080B ILM20128B DLM117664B；diff空白检查通过。
algo_motion_*、app_encoder_output_sample、app_motion_step已在ILM。替身不验证SPI硬件/齿隙/真实闭环稳定性。
