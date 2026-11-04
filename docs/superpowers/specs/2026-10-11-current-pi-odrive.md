# ODrive式电流PI模型整定

## 依据
已核对ODrive公开motor.cpp的update_current_controller_gains：
p_gain=current_control_bandwidth*phase_inductance；i_gain=(R/L)*p_gain。
公开run_calibration不执行本工程之前的200/300/500Hz阶跃搜索。
参考：https://github.com/odriverobotics/ODrive/blob/master/Firmware/MotorControl/motor.cpp
本工程独立C17适配，不复制C++代码。

## 行为变更
- 锁定启动时software.control.current_loop.bandwidth_rad_s，Kp=L*omega、Ki=R*omega。
- 非有限/非正带宽拒绝；omega*Ts>0.25拒绝，属于离散域工程约束，不是实测相位裕度。
- 单一目标带宽替代逐档搜索。生产YAML目标约500Hz；宿主对象测试目标200Hz。
- 保留16次3→3.5A阶跃采集和原指标门限，但step.passed只代表诊断合格。
- done表示RL与模型PI计算完成，不代表阶跃通过。数据无效/端口/拟合异常仍失败。
- Terminal完成及apply打印明确警告：诊断未通过、动态稳定性未验证。
- 结果不自动生效；OFF apply/save。CRC/版本/PWM/极对数恢复保留，FCUR格式不变。
- 诊断状态不持久化，上电restored仅代表参数恢复，不代表动态认证。
- 正常FOC算法不变，无新保护，无新增内存/任务。

## 测试
RL真实对象通过流程，PI等于辨识R/L乘目标omega；尾段偏差注入使阶跃诊断失败但模型结果完成；
正式参数未自动改动，显式OFF apply有效；非法/过高带宽拒绝并保持READY。
encoder/foc/control/identify两优化回归、make artifacts、diff check。

## 上板
先原有cal current/foc on/cal rl，记录模型参数与PI trial；失败diagnostic不得解释为振荡已经消除。
如需更保守模型，先停机，通过param设置software.control.current_loop.bandwidth_rad_s=1256.637
（200Hz），再辨识。默认500Hz不能因公式计算成功认定稳定。
先不save；OFF apply后零给定和小给定比较电流波形、纹波、ISR，确认后再save。
提示符复用上轮cal_rl_abort中的refresh；需上板验证实际刷新。
