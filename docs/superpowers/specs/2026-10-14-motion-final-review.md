# Motion当前提交版：最终审查与局部优化

基线 `c94e206`，Mephisto亲自审查；未委派代码判断。用户已实测双路ADC取帧与轴端角度正确。
以本文件及`2026-10-14-dual-encoder-cache.md`为当前实现说明；其余早期设计保留历史记录。

## 当前控制链
PWM1 CMP10→ADC0 + DMA_REQ0/HDMA3/SPI3 + DMA_REQ1/HDMA1/SPI1，均25kHz。
ADC唯一取帧者，发布同回调双路配对；SPI1失败不影响转子返回值。
GPTMR1 CH0=5kHz位置观测、CH1=10kHz运动控制；只读缓存，无SPI启动/等待/取帧。
motion力矩：轴端模型Nm/(50Kt)→iq；速度：轴端rad/s→PI→模型Nm→iq；正常id=0。
FOC统一id/iq请求、source/epoch/sequence、500us租约；校准/vtest/停止仍属于FOC。
位置P尚未实现，坏帧撤销输出、不自动恢复策略保持；全局保护动作开关保持0。

## 本轮修复
1. vtest能抢占MOTION输出，结束后旧请求可能恢复：MOTION source明确拒绝vtest；CALIB入口不变。
2. 强制角模式能选择MOTION：切source时拒绝非编码器角源；未消费的非零请求也不允许换source。
3. 500us请求过期后32bit mcycle回绕可复活旧请求：锁存过期，只有新提交/权限撤销清除。
4. 重复运动命令可刷新活动Kt/方向基准：拒绝与活动快照不一致的参数，不静默改变模型。
5. 撤销motion后torque/iq观测残留：统一清活动状态、积分、motion请求观测；不代表FOC缓降完成。
6. motion off总回报accepted：返回FOC停止请求结果；CALIB/vtest独占时不谎报正常停机成功。
7. 非法MOTN记录（大零位或direction=0）可恢复：显式语义校验及slot静态断言，Flash布局不变。
8. PI积分NAN可经fmin/fmax变成满幅输出：非有限积分/计算溢出清积分、零返回；边界测试覆盖。
9. 定时min/max永久累计，隐藏当窗状态：新增take_window、win计数、late本窗/累计；不清上次入口时刻。
10. pair分项计数跨ADC拍读取会出现valid>sample：短临界区读取三项计数的一致快照。
11. 增加motion中止/拒绝reason，成功新命令清零；正常恢复观测不会自动恢复控制输出。

关键错误写失败测试后修复；另增加跨圈、负方向、非法输入、Flash恢复及租约续签回归。
原参数/采样布局未迁移，不需重做机械相位及零点。

## 热路径优化
旧ELF反汇编确认motion/游标正常路径调用Flash的floorf/fabsf，及任务型参数查询。
单圈输入先验证[0,1)，相位[0,1/50)，使用有界单次归一化/整数取整/分支限幅替代libm。
标定相位相同数学关系、有界整数floor；样本非有限/越界先拒绝。
init缓存motor参数对象指针，运行仍读取RAM当前值并校验活动基准；无每拍getter懒初始化。
FOC epoch/state/stop只读热路径函数放ILM。反汇编检查motion、vernier、PI、delta无Flash库调用。
这是代码位置与调用消除的验证，不是实测耗时改善，需烧录比较。

## 新诊断
motion show：reason=0无/1样本/2重复/3游标残差/4周期/5参数/6FOC权限状态/7提交失败/8用户停止/9标定移动。
reason是最近一次拒绝或撤销，非独立故障锁存；后续另一失败可覆盖，成功新命令清零。
RTT：[MOTION] n累计、win本窗回调数、period/max本窗、late本窗/累计；单位cycle。
RTT反馈valid/age仍表示转子快照，不是双路解算/控制有效。motion show才是机械控制状态。

## 未关闭项（不宣称完整伺服验收）
- 用户报告直接iq运行时双路配对接受率约99.9954%，不是motion速度闭环验收。
- 偶发vernier拒绝仍撤销活动输出；本轮不改为自动恢复/容忍N拍。
- 速度PI仅自身限幅抗积分饱和，未接FOC执行摘要，电压/电流实际限幅时性能待验证。
- 不带硬件帧号/CS时间戳，ADC入口为时间代理；极端迟到跨拍仍需示波确认。
- 非抢占GPTMR会被ADC/USB/临界区延迟，采样dt和PI控制dt虽分离，实时抖动没有凭优化消失。
- 机械齿比49/50、减速50、开发电流1A/速度0.5rad/s仍固定；名义Kt、效率未标定。
- 机械零点为单圈坐标；相位圈编号存在7.2deg约定，置零独立且apply使旧零点失效。
- 电流1kHz动态诊断与电角校准验证失败问题未闭环，不保存失败结果。
- 当前新文件少量静态短名沿用开发版，不扩展为全工程风格重构。

## 上板聚焦回归
外部供电、低电流、先OFF：恢复Flash相位/零位、手动正反转/跨圈、pair采样计数。
对比[MOTION]新当窗max/late及ADC峰值；先验证角度和方向，再尝试低增益速度模式。
MOTION source时vtest必须拒绝；off/CALIB/重新enable后旧epoch不可输出。
修改Kt后重复命令应拒绝并由实时守卫撤销输出；测试后恢复原参数，不写Flash。
正常motion off异步缓降；校准期间motion off应报拒绝，不干扰校准。
本轮优化未烧录；此前成功日志不覆盖新增拒绝路径/库调用优化。

## 最新验证证据
执行：`for suite in encoder foc control identify; do bash scripts/tests/$suite/run.sh; done`。
两配置-O1与-O2 -ffast-math：encoder168/foc289/control452218/identify77 checks，0失败。
encoder/control另以`-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all`跑两遍，0失败。
`make artifacts`成功、构建日志无warning/error；`git diff HEAD --check`通过。
FLASH206944B，ILM20752B，DLM117696/130304B（90.32%）。
nm：vernier=0x1f12，speed PI=0x2024，配对消费=0x29ae，ADC取帧=0x2a44，motion=0x4896。
objdump复核motion/PI/vernier/delta直接调用无0x800... Flash目标，正常热路径仅ILM调用。
改动不自动提交/烧录；准备阶段提交信息`/tmp/opencode/motion-final-review-commit-message.txt`。
