# ODrive思路适配：RL/PI校准

## 来源与移植边界
核对公开ODrive motor.cpp中的ResistanceMeasurementControlLaw、InductanceMeasurementControlLaw、
update_current_controller_gains：独立积分调压、交替电压累积电流变化、Kp=L*w和Ki=R*w。
参考：https://github.com/odriverobotics/ODrive/blob/master/Firmware/MotorControl/motor.cpp
本实现为算法思路独立C17适配，不复制C++、硬件控制器或异常停机体系；不是ODrive逐行移植。

## Rs
任务每5ms使用已有32Hz电流均值积分调压：V+=0.005*(目标I-均值I)。
目标2/3/4/3/2A，电压指令域0~1.5V（不构成相电流硬限流）。600ms后冻结电压，20块128点均值回归V=RI+b。
正常电流PI不参与，保持长窗口q均值/漂移质量检查。

## Ld
保持正3A恒压偏置；ADC ISR在128拍捕获中每4/8拍切换偏置±0.6V，两种半周期各16块对齐平均。
与ODrive每拍±V不同：为了信噪比和正偏置采用多拍激励，并计入Rs压降。
排除每次切换后的两拍，按电压极性分组：
 L=Ts*(mean(v-R*(i[k]+i[k-1])/2)_high - mean(...)_low)
       /(mean(deltaI)_high-mean(deltaI)_low)。
共同恒定偏差通过分组差抵消；不按带噪deltaI正负分组（会产生选择偏差）。
报告half4/half8电感、一致性、增量模型残差。原指数拟合仅纯算法诊断保留，不再进主流程。
实际PWM生效到ADC采样区间尚未上板校验；跳过边缘不能替代时序测量。

## PI与隔离
公式及200/300/500Hz的16次阶跃验收保留，失败候选指标独立发布；不修改foc_current_step。
现有app_io调度、CALIB互斥、有限恒压租约、退出恢复、OFF apply/save及FCUR恢复保持。
不新增保护关机，5A只是用户批准的激励设计边界；±0.6V实际电流纹波仍需上板测量。

## 验证
- 4拍交替精确RL对象120uH±6uH。
- 8拍交替含0.1V共同偏差及16次平均残余噪声120uH±25uH。
- 非法Ts/无效数据拒绝；真实执行端口捕获结束回偏置；完整真实电流环对象流程。
- encoder/foc/control/identify回归，make artifacts，diff check。
未验证：实际脉冲时序、真实死区误差、实际相电流峰值、上板辨识精度和候选PI可通过性。

## 上板步骤
OFF cal current → foc on → cal rl；先不apply/save，做3次重复。
记录R、half4、half8、repeat、fitL、PI trial及RTT；不能把failed的L均值当成可应用结果。
若half4/half8系统性不符，先检查PWM装载/ADC时间，不放宽20%门限。
通过后OFF apply/save，重启应restored(flash)。
