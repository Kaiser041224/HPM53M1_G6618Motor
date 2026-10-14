# 双编码器同步采样：当前实现

> 后续提交前修复及诊断窗口变更见 `2026-10-14-motion-final-review.md`；本文件记录采样架构。

## 生效数据流（代码实现，硬件尚待本轮烧录确认）
PWM1 CMP10共同触发：ADC0三相电流、DMA_REQ0/HDMA3/SPI3转子、DMA_REQ1/HDMA1/SPI1游标。
ADC ISR调用app_encoder_sync_sample：取转子帧并执行既有快照策略；取游标帧；同一次发布rotor/output配对。
GPTMR1 CH0=5kHz位置入口、CH1=10kHz速度入口，均纯reload中断；CH2/3不运行。
app_encoder_output_sample现在只复制ADC发布配对，不访问SPI，不把新游标与旧转子字段拼接。
任一路失败均配对无效；SPI1失败不改变转子返回值、不阻断电流控制。GPTMR取最新快照，跳过中间快照是预期行为。

## 状态与所有权
- SPI3/SPI1运行期唯一取帧/补TX FIFO者为ADC ISR，两个DMA独立并行触发。
- s_encoder_pair单一ADC写者；GPTMR读者短临界区整体复制，无无限自旋。
- 最近成功游标raw独立发布，g_enc_output_deg不受转子质量拒绝阻断；不是实际出轴角。
- g_motion_shaft_angle_rad/deg为游标换算并应用机械零点后的轴端角；valid标记必须一并观察。
- 转子与配对timestamp相同，沿用ADC ISR入口代理时间，不是真实CS时间戳。
- 测速用最新/上一配对timestamp差；PI积分使用GPTMR控制入口间隔，两种dt明确分开。

## 已移除
- SPI1 GPTMR1 OUT2触发、CH2比较输出、CH3延后比较消费。
- compare_irq_us、sync_once、GPTMR1 OUT2新增契约/路由。
- 初始化单次SYNCI等待/全局关IRQ最多60us。
Interface及drv_trgm恢复本轮扩展前内容；驱动保留既有ILM分发、NULL回调配置、GPTMR1优先级2。
不恢复2rad/s速度拒绝门限，不修改相位/机械零位Flash布局。

## 验证重点
宿主：两路PWM路由、ADC恰好一次游标fetch、缓存多次读取无设备访问、失败不拼旧帧、SPI1失败不影响转子、
超龄拒绝；不同GPTMR延迟/相同样本时间差得到同速度；timer部分启动失败停止已启动通道。
宿主无法证明DMA启动时差、RX FIFO内帧号、ISR最坏耗时和实际闭环稳定性。

## 上板（外部电源、先FOC OFF）
1. 启动日志：[ENC] sync OK、vernier sync started、MOTION started。
2. [ENC] sync及pair的output rate约25000/报告秒，output fail接近0、pair valid接近output rate。
3. [MOTION] pos约5000/s、vel约10000/s；g_enc_output_raw/deg和轴端角度应更新。
4. 两路均每25kHz排空FIFO，g_spi_trig_stale_drop不再有15k/s的正常降采样增长。
5. 示波测两CS同PWM参考、周期40us；对比ADC ISR/step最大耗时是否增加及是否超过40us。
6. motion show重复读取，看sample/duplicate/vernier/timing增量；未证实数据健康前不启用速度模式。
7. 原foc on/低电流/off、cal encoder/cal rl及取消回归；不保存失败校准结果。

额外SPI1取帧+发布有执行开销，不能据编译/宿主测试声称零负担。正常无SPI1等待，但极端IRQ延迟、漏触发或跨拍仍需观测确认。

## 本轮验证结果
encoder165/foc289/control452159/identify77 checks，-O1与-O2 -ffast-math全部0失败。
make artifacts成功，构建日志无warning/error，git diff/HEAD diff空白检查通过。
FLASH206680B、ILM20496B、DLM117696B（90.32%）；取帧/配对消费/motion实时入口位于ILM。
未烧录；此前独立10kHz采样版本的硬件成功日志不能代替本版本验证。
GPTMR仍可能因非抢占IRQ/任务临界区出现入口抖动；PI控制周期异常、单次坏样本撤销策略仍保留。
