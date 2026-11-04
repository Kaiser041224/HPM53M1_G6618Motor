# RL/PI 当前实现与提交前审查

本文件为当前实现摘要。2026-10-11各设计文档及2026-10-12电压范围文档保留为演进记录，
其中早期的0.70默认、500Hz目标、指数Ld主路径及PI逐档验收已被后续实现替代。

## 当前行为
- 默认25kHz执行、1kHz目标电流带宽；手册初值Kp=0.744557459、Ki=992.743278534。
- dutycap默认0.95，可在OFF以RAM配置0.70~0.98；还受software.duty_max限制。
- cal rl：同PWM零电压基线 → 独立慢积分Rs平台 → 固定角4/8拍交替Ld段积分 → 单目标PI。
- Rs20块×128点，比较前后半窗口均值；Ld每脉宽64块，残差拒绝时累计追加到128块，最多一次。
- PI按Kp=L*w、Ki=R*w计算，16次3→3.5A诊断，每次捕获前置基线；诊断不通过会警告，不等价于计算失败。
- done仅表示模型整定完成，不证明相位裕度或原始波形合格；OFF apply/save，FCUR在init前恢复。
- 正常终端off：100个任务拍给定缓降、20拍零给定后关桥；不是速度闭环制动，也不保证母线不反灌。
- 没有新增保护关机策略。恢复FAULT清锁存的旧语义不等于开启全局保护。

## 本轮审查修复
1. 软停止后FAULT无法通过foc off恢复：FAULT请求走原即时disable。
2. 校准/软停止期间可从低层入口启动vtest：新增互斥门控。
3. 失败电流帧被丢弃后拼接捕获：保护零矢量路径冻结无效缓冲并撤销校准电压租约；好帧不能洗掉无效标记。
4. 开环校准绕过正常PI输入净化：非有限电流立即执行已有零矢量回退，不继续开环输出。
5. apply/save可能使用变更后的PWM/极对数：快照并校验辨识条件；save拒绝事后param修改的结果；restore只允许OFF。
6. 阶跃纹波以目标值初始化极值，恒定偏差被误算为纹波：从首个尾段实际样本初始化。
7. Terminal快照跨拍撕裂：短临界区整体复制；新端口声明纳入原extern C块。

## 验证策略
宿主control真实实现：vtest互斥、捕获中坏帧/NAN、坏帧后恢复、参数编辑后save拒绝、
变频后apply拒绝、运行时restore拒绝、恒定偏差不虚报纹波。
其余encoder/foc/identify回归。FAULT清锁存分支当前全局保护动作关闭，未做上板故障注入。

## 已知未闭环项（不宣称生产稳定性）
- 真实三相测量和约1A的偏差未解释；固定角q偏差及转子位移判据仍会中止校准。
- 1kHz阶跃超调/纹波诊断未达标。apply允许模型参数，不等价于动态验证通过。
- PWM on_modify三相顺序写，前一拍电压配对是工程模型；尚无寄存器级或示波端到端时序验证。
- 本次捕获修复覆盖已检测失败帧，不能检测完全没有回调的ADC漏拍。
- app_io预算≤200us、实际栈高水位尚需测量；Ld局部数组约1.5KB，任务栈6KB。
- 软停止只斜坡给定，没有电流衰减/转速为零闭环判据；故障与Ozone即时停机不受斜坡延迟。
- 锁相同步噪声不会因多次平均必然消失；没有把三相基线自动扣除。
- 历史motor.yaml手册RMS峰值口径问题不在本次范围，禁止将当前限流配置当作验证过的硬件保护。

## 上板聚焦回归
1. cal rl期间vtest拒绝；正常off pending期间也拒绝vtest和新给定。
2. 正常校准、取消、off/apply/save/reboot；修改参数后save应拒绝。
3. 相同1kHz/0.95设置核对新旧Rs/Ld与PI指标，记录ISR最大耗时与io栈高水位。
4. 不对输出中的OK作动态性能认证；记录所有WARN与失败阶段。

## 本轮验证记录
2026-10-12：encoder127/foc289/control451964/identify77 checks，-O1与-O2 -ffast-math均0失败。
control另使用UBSan（-fsanitize=undefined -fno-sanitize-recover=undefined）两遍通过。
make artifacts成功、无warning/error、git diff及cached diff空白检查通过。
FLASH201984B，ILM16096B，DLM117664/130304B（90.30%）。
nm：app_current_identify_run_once=0x8001486e、id_rl_pulse_inductance=0x8000f642（Flash）；
app_foc_current_run_fresh=0x3050（ILM）。仅证明位置，不代替运行时耗时测试。
这些check多数是物理替身逐拍断言，不是同数量独立用例。新增review修复未上板。
