# GPTMR独立游标触发

> 历史试验，已停用。当前实现见 `2026-10-14-dual-encoder-cache.md`：两路PWM25kHz同触发、ADC取帧发布配对、GPTMR仅编排外环。下文仅保留试验演进记录。

替代先前SPI1 PWM25kHz采样/10kHz消费。SPI3/电流ADC25kHz保持不变。
GPTMR1 CH2=10kHz比较输出，CMP0=8us、CMP1=周期；低初值，设计为8us上升沿触发。
CH2 OUT2→TRGM DMA_REQ1→HDMA1→SPI1 CMD。CH3 CMP0=20us中断消费，设计留12us传输裕量。
CH0保持5kHz位置入口，CH1停止，CH2无IRQ。实际输出极性、频率/相位需示波确认。

启动一次SYNCI：PWM1 CH10REF→GPTMR1 SYNCI，启用CH0/2/3上升同步、启动计数，
监视CH2计数回退，在60us窗口内确认后立即禁用同步边沿；运行期不逐PWM拍复位GPTMR。
只允许初始化或停桥窗口调用；轮询屏蔽全局IRQ最多60us，会影响启动期ADC/tick，不能运行中重启。
回退检测依赖CH2=100us周期，60us小于自然reload，防止把自然回绕误认为同步。
首边沿若过早未观察到回退，等待下一PWM边沿；超时停止motion通道。

局限：没有双SPI帧号；游标采样设计在PWM参考之后8us，转子快照对应另一时间，不能宣称严格同拍。
控制速度仍取入口时间，硬件时间戳配对为后续工作。不增加取帧忙等，不恢复2rad/s拒绝门限。
样本busy/sample仍增长时应读SPI1独立计数/测SPI CS，不能继续扩大等待。

验证：宿主检查SPI1路由为GPTMR1 OUT2、CH2频率、CH3比较相位、同步失败停止。
硬件：先OFF启动，确认[MOTION]started；示波SPI1 CS=10kHz、SPI3 CS=25kHz；
取帧max/busy/sample、stale每秒增量、ADC最大周期及USB压力测试。stale不再预期15000/s。
上板确认前不启用速度闭环。旧提交暂存与新改动不同，禁止直接提交旧index。
