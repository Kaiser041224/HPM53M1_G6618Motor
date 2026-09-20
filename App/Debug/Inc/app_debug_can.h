#ifndef APP_DEBUG_CAN_H
#define APP_DEBUG_CAN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CAN 自检初始化：
 *        1) 内部环回自检（验证控制器 / 消息 RAM / 过滤器 / 中断链路，无需外部节点）
 *        2) 以经典 CAN 模式初始化 MCAN3（正常运行波特率来源 config/software.yaml；
 *           环回自检固定 1Mbps）
 *        3) 配置接收过滤器（全接收）与 RX 回调
 */
void app_debug_can_init(void);

/**
 * @brief CAN 自检周期任务：
 *        1) 分发接收回调（主循环上下文）：打印 + **原样回显到总线**
 *        2) 每秒发送一帧经典 CAN（ID 取自 config/software.yaml → sw.can.tx_report_id，
 *           8 字节，首字节为递增计数）
 *        3) 每秒打印状态行（发送结果 / 错误计数 / bus off / 收包统计）
 */
void app_debug_can_run_once(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_CAN_H */
