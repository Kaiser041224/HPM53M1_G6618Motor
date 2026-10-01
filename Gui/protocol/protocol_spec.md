# Gui ↔ 电机控制板 通讯协议规范 v1.1

本文件是 `motor_proto.h` 的配套说明，两者共同构成 Gui 上位机与电机控制板之间的
**线格式契约**。任何一端实现编解码、传输解析、会话状态机时，都以本规范为准；
一旦与 `motor_proto.h` 冲突，以 `motor_proto.h` 的宏/枚举定义为准。

> **v1.1 为 v1 的增量（additive）**：不改变既有帧格式、`VER`、状态码与金向量，
> 仅在 `CTRL_CMD` 内新增 3 个 opcode（见 §7）。固件侧实现待 M2；当前 sim / 主机
> 已按本规范实现。旧端对未知 opcode 的降级约定见 §7.4。

- 传输层字节序：**全部小端（little-endian）**。
- 版本：`MP_VERSION = 0x01`（v1.1 复用，线格式不变）。
- 最大载荷：`MP_MAX_PAYLOAD = 512` 字节。
- 单帧最大线长：`2(SOF) + 5(hdr) + 512(payload) + 2(CRC) = 521` 字节。

---

## 1. 帧格式（Frame Format）

### 1.1 字段布局

| 偏移 | 字段 | 长度 | 类型 | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| 0 | `SOF0` | 1 | u8 | 恒 `0xA5` |
| 1 | `SOF1` | 1 | u8 | 恒 `0x5A`（线序 `A5 5A`，即 `MP_SOF0` `MP_SOF1`） |
| 2 | `VER` | 1 | u8 | 恒 `0x01`（`MP_VERSION`） |
| 3 | `CMD` | 1 | u8 | 命令字；响应 = 请求 CMD `\| 0x80` |
| 4 | `SEQ` | 1 | u8 | 序号，请求/响应配对，u8 回绕（255→0） |
| 5 | `LEN` | 2 | u16 LE | `PAYLOAD` 字节数，范围 `0..512` |
| 7 | `PAYLOAD` | `LEN` | bytes | 命令体；响应首字节为 `mp_status_t` |
| 7+LEN | `CRC16` | 2 | u16 LE | CRC16-CCITT-FALSE，覆盖 `VER..PAYLOAD`（偏移 2 .. 6+LEN） |

### 1.2 SOF 与重同步

`SOF` 是 2 字节魔数，线序固定为 `A5 5A`。解析器在字节流中扫描第一个 `0xA5`，
若其后不是 `0x5A`，则丢弃该字节、从下一个字节继续扫描（每步仅前进 1 字节）。

### 1.3 CRC16-CCITT-FALSE

| 参数 | 值 |
| :--- | :--- |
| 算法 | CRC-16/CCITT-FALSE |
| 多项式 poly | `0x1021` |
| 初值 init | `0xFFFF` |
| 输入反转 refin | 否 |
| 输出反转 refout | 否 |
| 结果异或 xorout | `0x0000` |
| 校验值（`"123456789"`） | `0x29B1` |

计算范围：自 `VER`（偏移 2）起，至 `PAYLOAD` 最后一个字节（偏移 `6+LEN`）止。

参考实现（C17）：

```c
uint16_t mp_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

CRC 为 16-bit 无符号整数，**小端放置**：偏移 `7+LEN` 存低字节，`8+LEN` 存高字节。

### 1.4 响应规则

- 响应 `CMD = 请求 CMD | MP_RESPONSE_BIT (0x80)`。
- 响应 `PAYLOAD` 首字节恒为 `mp_status_t`，其后为对应命令的响应体。
- 响应 `SEQ` 必须与请求 `SEQ` 相同（u8 回绕）。
- 请求（`CMD < 0x80`）的 `PAYLOAD` 首字节为命令体，不含状态码。

---

## 2. 金帧逐步字节注解（`test_vectors.json`）

以下 4 个金帧取自 `Gui/protocol/test_vectors.json`，逐字节注解。
金向量与契约一致（历史勘误见附录 A）。

### 2.1 `proto_info_req_seq0`

```
A5 5A  01  01  00  0000  E9 CD
```

| 偏移 | 字节 | 字段 | 值 / 含义 |
| :--- | :--- | :--- | :--- |
| 0 | `A5` | SOF0 | — |
| 1 | `5A` | SOF1 | — |
| 2 | `01` | VER | 1 |
| 3 | `01` | CMD | `MP_CMD_PROTO_INFO` |
| 4 | `00` | SEQ | 0 |
| 5–6 | `00 00` | LEN | 0（无载荷） |
| 7–8 | `E9 CD` | CRC16 | 值 `0xCDE9`（小端 `E9 CD`），覆盖 `01 01 00 00 00` |

### 2.2 `proto_info_resp_sim`

```
A5 5A  01  81  00  0F00  00 01 00 03 000000 07 67 75 69 2D 73 69 6D  CF 5D
```

| 偏移 | 字节 | 字段 | 值 / 含义 |
| :--- | :--- | :--- | :--- |
| 0 | `A5` | SOF0 | — |
| 1 | `5A` | SOF1 | — |
| 2 | `01` | VER | 1 |
| 3 | `81` | CMD | `MP_CMD_PROTO_INFO \| 0x80`（响应） |
| 4 | `00` | SEQ | 0 |
| 5–6 | `0F 00` | LEN | 15 |
| 7 | `00` | 载荷[0] | `status = MP_STATUS_OK` |
| 8 | `01` | 载荷[1] | `proto_major = 1` |
| 9 | `00` | 载荷[2] | `proto_minor = 0` |
| 10–13 | `03 00 00 00` | 载荷[3..6] | `caps = 0x00000003`（bit0\|bit1） |
| 14 | `07` | 载荷[7] | `name_len = 7` |
| 15–21 | `67 75 69 2D 73 69 6D` | 载荷[8..14] | 设备名 `"gui-sim"`（ASCII） |
| 22–23 | `CF 5D` | CRC16 | 值 `0x5DCF`（小端 `CF 5D`），覆盖 `01 81 00 0F 00` + 上述载荷 |

### 2.3 `param_read_req_motor_pole_pairs`

```
A5 5A  01  04  01  1100  10 6D6F746F722E706F6C655F7061697273  35 40
```

| 偏移 | 字节 | 字段 | 值 / 含义 |
| :--- | :--- | :--- | :--- |
| 0 | `A5` | SOF0 | — |
| 1 | `5A` | SOF1 | — |
| 2 | `01` | VER | 1 |
| 3 | `04` | CMD | `MP_CMD_PARAM_READ` |
| 4 | `01` | SEQ | 1 |
| 5–6 | `11 00` | LEN | 17 |
| 7 | `10` | 载荷[0] | `name_len = 16` |
| 8–23 | `6D 6F 74 6F 72 2E 70 6F 6C 65 5F 70 61 69 72 73` | 载荷[1..16] | ASCII `"motor.pole_pairs"` |
| 24–25 | `35 40` | CRC16 | 值 `0x4035`（小端 `35 40`），覆盖头 + 载荷 |

### 2.4 `param_read_resp_bad_param_seq1`

```
A5 5A  01  84  01  0100  02  A0 81
```

| 偏移 | 字节 | 字段 | 值 / 含义 |
| :--- | :--- | :--- | :--- |
| 0 | `A5` | SOF0 | — |
| 1 | `5A` | SOF1 | — |
| 2 | `01` | VER | 1 |
| 3 | `84` | CMD | `MP_CMD_PARAM_READ \| 0x80`（响应） |
| 4 | `01` | SEQ | 1 |
| 5–6 | `01 00` | LEN | 1 |
| 7 | `02` | 载荷[0] | `MP_STATUS_BAD_PARAM` |
| 8–9 | `A0 81` | CRC16 | 值 `0x81A0`（小端 `A0 81`），覆盖 `01 84 01 01 00 02` |

---

## 3. 命令表

`CMD` 值来自 `mp_cmd_t`。所有多字节字段小端；`f32` 为 IEEE-754 单精度小端；
`value u32` 为原始位型（按 `type` 重解释）。

### 3.1 `MP_CMD_PROTO_INFO = 0x01`（版本协商/能力位/设备名）

**请求载荷**：空（`LEN = 0`）。

**响应载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `status` | u8 | `mp_status_t` |
| 1 | `proto_major` | u8 | 主版本，v1 恒为 `0x01` |
| 2 | `proto_minor` | u8 | 次版本 |
| 3–6 | `caps` | u32 LE | 能力位，见 §6 |
| 7 | `name_len` | u8 | 设备名长度（≤ 32） |
| 8.. | `name` | u8[] | ASCII 设备名 |

### 3.2 `MP_CMD_PARAM_META_COUNT = 0x02`（元数据条目数）

**请求载荷**：空。

**响应载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `status` | u8 | `mp_status_t` |
| 1–2 | `count` | u16 LE | 参数元数据条目数 |

### 3.3 `MP_CMD_PARAM_META_GET = 0x03`（按序号取元数据）

**请求载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0–1 | `index` | u16 LE | 元数据序号，`0..count-1` |

**响应载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `status` | u8 | 越界返回 `MP_STATUS_RANGE_ERR` |
| 1–2 | `index` | u16 LE | 回显序号 |
| 3 | `type` | u8 | `mp_type_t` |
| 4 | `apply` | u8 | `mp_apply_t` |
| 5–8 | `min` | f32 LE | 合法下界 |
| 9–12 | `max` | f32 LE | 合法上界 |
| 13 | `name_len` | u8 | 名称长度 |
| 14.. | `name` | u8[] | ASCII 参数名 |
| +0 | `unit_len` | u8 | 单位串长度（可为 0） |
| +1.. | `unit` | u8[] | ASCII 单位 |

### 3.4 `MP_CMD_PARAM_READ = 0x04`（按名读参数）

**请求载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `name_len` | u8 | 参数名长度 |
| 1.. | `name` | u8[] | ASCII 参数名 |

**响应载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `status` | u8 | 未知名字返回 `MP_STATUS_BAD_PARAM` |
| 1 | `type` | u8 | `mp_type_t` |
| 2–5 | `value` | u32 LE | 原始位型，按 `type` 重解释 |

### 3.5 `MP_CMD_PARAM_WRITE = 0x05`（按名写参数）

**请求载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `name_len` | u8 | 参数名长度 |
| 1.. | `name` | u8[] | ASCII 参数名 |
| +0 | `type` | u8 | `mp_type_t` |
| +1–4 | `value` | u32 LE | 原始位型 |

**响应载荷**：`status`（u8）。类型不符 `MP_STATUS_BAD_PARAM`，越界 `MP_STATUS_RANGE_ERR`。

### 3.6 `MP_CMD_PARAM_COMMIT = 0x06`（参数落盘，停机窗口联锁）

**请求载荷**：空。

**响应载荷**：`status`（u8）。未停机或故障锁存时返回 `MP_STATUS_LOCKED`。

### 3.7 `MP_CMD_STATUS_GET = 0x07`（运行状态快照）

**请求载荷**：空。

**响应载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `status` | u8 | `mp_status_t` |
| 1–4 | `fault_flags` | u32 LE | 实时故障位 |
| 5 | `motor_running` | u8 | `0`/`1` |
| 6 | `motor_state` | u8 | `MP_MOTOR_STOP` / `MP_MOTOR_RUN` |
| 7–10 | `freq_hz` | f32 LE | 电频率（Hz） |
| 11–14 | `mod_pct` | f32 LE | 调制比（%） |
| 15–18 | `i_u` | f32 LE | U 相电流 |
| 19–22 | `i_v` | f32 LE | V 相电流 |
| 23–26 | `i_w` | f32 LE | W 相电流 |
| 27–30 | `vbus` | f32 LE | 母线电压 |
| 31–34 | `ntc_temp` | f32 LE | NTC 温度 |
| 35–38 | `enc_rotor_deg` | f32 LE | 转子角（度） |
| 39–42 | `enc_output_deg` | f32 LE | 输出轴角（度） |

### 3.8 `MP_CMD_CTRL_CMD = 0x08`（电机控制，联锁同固件 terminal）

**请求载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `opcode` | u8 | `mp_ctrl_opcode_t` |
| 1–4 | `value` | f32 LE | 参数值；`START`/`STOP` 忽略 |

`opcode`：`MP_CTRL_START=0x01`、`MP_CTRL_STOP=0x02`、`MP_CTRL_SET_FREQ=0x03`、
`MP_CTRL_SET_MOD=0x04`；v1.1 增量 `MP_CTRL_SET_OUTPUT_ZERO_NOW=0x05`、
`MP_CTRL_SET_OUTPUT_ZERO_ABS=0x06`、`MP_CTRL_SET_MODE=0x07`（详见 §7）。

**响应载荷**：`status`（u8）。联锁拒绝返回 `MP_STATUS_LOCKED`。

### 3.9 `MP_CMD_FAULT_GET = 0x09`（故障与锁存）

**请求载荷**：空。

**响应载荷**：

| 序号 | 字段 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| 0 | `status` | u8 | `mp_status_t` |
| 1–4 | `fault_flags` | u32 LE | 实时故障位 |
| 5–8 | `latched_flags` | u32 LE | 锁存故障位 |

### 3.10 `MP_CMD_FAULT_CLEAR = 0x0A`（清除故障锁存）

**请求载荷**：空。

**响应载荷**：`status`（u8）。

### 3.11 `MP_CMD_STREAM_SUB = 0x0B` / `MP_CMD_STREAM_UNSUB = 0x0C`（v1 预留）

请求载荷：空。响应载荷：`status = MP_STATUS_UNSUPPORTED`。v1 不实现一级流。

---

## 4. 状态码表（`mp_status_t`）

| 值 | 名称 | 语义 |
| :--- | :--- | :--- |
| `0x00` | `MP_STATUS_OK` | 成功 |
| `0x01` | `MP_STATUS_BAD_CMD` | 未知命令字 / 命令体长度不符 |
| `0x02` | `MP_STATUS_BAD_PARAM` | 参数名不存在或类型不符 |
| `0x03` | `MP_STATUS_BUSY` | 目标暂时忙，可重试 |
| `0x04` | `MP_STATUS_LOCKED` | 联锁拒绝，满足以下任一：**① 存在未清除的故障锁存**；**② 需要停机窗口的操作（如 `PARAM_COMMIT`）在电机未停机时下发** |
| `0x05` | `MP_STATUS_UNSUPPORTED` | 命令/能力在当前版本或当前设备不可用（v1 的流命令恒返回此值） |
| `0x06` | `MP_STATUS_RANGE_ERR` | 数值越界（如 meta 序号越界、写入超 `[min,max]`） |

---

## 5. 传输映射（Transport Mapping）

协议帧本身是面向字节流的、无转义的自同步格式；不同物理链路只负责把字节送达。

### 5.1 bulk / USB CDC / UART（直载）

- 帧字节原样写入/读出，无转义、无二次封装。
- 接收端使用 §1.2 的 SOF 扫描 + §1.3 CRC 校验完成分帧。
- 分帧流程：
  1. 扫描到 `A5 5A`；
  2. 读取 5 字节头（`VER/CMD/SEQ/LEN`），校验 `VER == 0x01` 且 `LEN <= 512`；
  3. 读取 `LEN + 2` 字节（载荷 + CRC）；
  4. 校验 CRC；失败则丢弃首字节、回到步骤 1 重同步。
- 无状态、可乱序到达无关帧；请求/响应通过 `SEQ` 配对。

### 5.2 CAN 适配器（分片承载）

CAN 经典帧单帧数据 ≤ 8 字节，无法容纳最大 521 字节的协议帧，因此在 CAN 与协议
解析之间引入一个**适配器层**做分片/重组：

- 每个 CAN 数据帧载荷布局：`[frag_index u8][frag_total u8][data 0..6 bytes]`。
- 同一协议帧的所有分片使用同一 CAN ID；`frag_index` 从 `0` 递增，`frag_total` 为分片总数。
- 重组端按 `frag_index` 收集，直到 `frag_total` 片齐全，拼接成完整协议帧后再交给 §5.1 解析器。
- 分片/重组属于**适配器职责**，不改变协议线格式；`motor_proto.h` 不感知分片。
- 丢片超时：若在 `T_reasm`（由适配器定义）内未收齐，丢弃该组并复位重组缓冲。

> v1 仅要求适配器无损搬运；具体 CAN ID 分配与超时值由 CAN 命令模块单独规定。

---

## 6. 版本协商与能力位

### 6.1 流程

1. 主机连接后首先发送 `MP_CMD_PROTO_INFO` 请求（空载荷）。
2. 设备返回 `proto_major` / `proto_minor` / `caps` / `name`。
3. 主机判定：
   - **`proto_major != 0x01` → 主机必须拒绝连接并报告错误**（主版本不兼容，禁止继续通讯）。
   - `proto_major == 0x01` 且 `proto_minor` 不同 → 允许连接；次版本差异视为向后兼容，
     未知的次版本特性由能力位约束，主机忽略不认识的能力位。
4. 若设备不支持某能力（对应 `caps` 位为 `0`），主机**不得**下发该命令；
   若仍下发，设备返回 `MP_STATUS_UNSUPPORTED`。

### 6.2 `caps` 位定义

| 位 | 名称 | 含义 |
| :--- | :--- | :--- |
| 0 | `CAP_PARAM_WRITE` | 支持 `MP_CMD_PARAM_WRITE` |
| 1 | `CAP_CONTROL` | 支持 `MP_CMD_CTRL_CMD` |
| 2 | `CAP_STREAM` | 一级流订阅（**v1 预留，恒为 0**） |
| 3..31 | — | 保留，v1 恒 `0`；接收端忽略未知位 |

金帧 `proto_info_resp_sim` 中 `caps = 0x00000003`，即 `CAP_PARAM_WRITE | CAP_CONTROL`。

---

## 7. v1.1 增量：出轴零点与模式（`MP_CMD_CTRL_CMD` 内）

v1.1 不新增命令字、不改变帧格式与 `STATUS_GET` 布局，仅在 `MP_CMD_CTRL_CMD = 0x08`
（§3.8）内扩展 opcode。请求体恒为 `opcode u8 | value f32 LE`（5 字节）。

### 7.1 opcode 定义

| opcode | 名称 | `value` 语义 |
| :--- | :--- | :--- |
| `0x05` | `MP_CTRL_SET_OUTPUT_ZERO_NOW` | 忽略（传 `0.0`）。把**当前出轴角**设为零点，`enc_output_deg` 显示自此偏移归零 |
| `0x06` | `MP_CTRL_SET_OUTPUT_ZERO_ABS` | 指定角度（度），把该角度设为出轴零点 |
| `0x07` | `MP_CTRL_SET_MODE` | 以 f32 位型低字节承载 u8：`1`=力矩 / `2`=速度 / `3`=角度 |

要点：

- **仅出轴**：零点命令作用于 `enc_output_deg` 合成显示，**不**改变 `enc_rotor_deg`。
  转子零点属电机·驱动域，不在本协议命令内。
- `MP_CTRL_SET_MODE` 的模式**仅存设备状态**：`STATUS_GET` 布局不变、无回读字段
  （UI 侧自行记录当前模式）。
- 显示角合成关系（sim 参考实现）：`enc_output_deg = (tick_raw − zero) mod 360`，
  其中 `NOW` 令 `zero = 当前 tick_raw`，`ABS` 令 `zero = value mod 360`。

### 7.2 状态码与联锁语义

| 场景 | 返回 `status` |
| :--- | :--- |
| 停机且无故障锁存，零点写入 / 模式切换 | `MP_STATUS_OK (0x00)` |
| 运行中或存在未清除故障锁存，下发零点 / 模式 | `MP_STATUS_LOCKED (0x04)` |
| `MP_CTRL_SET_OUTPUT_ZERO_ABS` 的 `value` 为 `NaN`/`±Inf` | `MP_STATUS_RANGE_ERR (0x06)` |
| `MP_CTRL_SET_MODE` 模式非 `1/2/3` | `MP_STATUS_BAD_CMD (0x01)` |
| 未知 opcode | `MP_STATUS_BAD_CMD (0x01)` |

零点写入与模式切换均属**停机窗口操作**，联锁语义同 `PARAM_COMMIT`（§3.6）。

### 7.3 帧示例

以下帧可作实现自检（CRC16-CCITT-FALSE，范围 `VER..PAYLOAD`，小端放置）。

**`SET_OUTPUT_ZERO_NOW` 请求（`SEQ=0`）**：

```
A5 5A  01  08  00  0500  05 00 00 00 00  73 FD
```

| 偏移 | 字节 | 含义 |
| :--- | :--- | :--- |
| 2 | `01` | `VER` |
| 3 | `08` | `CMD = MP_CMD_CTRL_CMD` |
| 4 | `00` | `SEQ = 0` |
| 5–6 | `05 00` | `LEN = 5` |
| 7 | `05` | `opcode = MP_CTRL_SET_OUTPUT_ZERO_NOW` |
| 8–11 | `00 00 00 00` | `value = 0.0f`（忽略） |
| 12–13 | `73 FD` | CRC16 `0xFD73` |

**成功响应（`SEQ=0`）**：`A5 5A 01 88 00 0100 00 7D 5C`（`status=OK`，CRC `0x5C7D`）。

**运行中拒绝响应（`SEQ=0`）**：`A5 5A 01 88 00 0100 04 F9 1C`（`status=LOCKED`，CRC `0x1CF9`）。

**`SET_OUTPUT_ZERO_ABS` 请求（`value=90.0`，`SEQ=1`）**：

```
A5 5A  01  08  01  0500  06 00 00 B4 42  3D EE
```

（`90.0f` 位型 `0x42B40000`，小端 `00 00 B4 42`；CRC `0xEE3D`。）

**`SET_MODE` 请求（`mode=2`，`SEQ=2`）**：

```
A5 5A  01  08  02  0500  07 02 00 00 00  3E DB
```

（f32 位型低字节 `0x02`；CRC `0xDB3E`。）

### 7.4 v1.1 兼容性与协商

- **线格式向后兼容**：未使用新 opcode 的 v1.0 流量与金向量完全不受影响
  （`test_vectors.json` 不变）。
- **旧端（固件 v1.0）收到新 opcode**：
  - 若仅按“未知 opcode”处理 → 返回 `MP_STATUS_BAD_CMD (0x01)`；
  - 若设备显式声明不支持 → 返回 `MP_STATUS_UNSUPPORTED (0x05)`。
- **主机容错约定**：v1.1 不新增 `caps` 位（仍沿用 `CAP_CONTROL`）。主机下发新 opcode
  时，须同时容忍 `BAD_CMD` 与 `UNSUPPORTED` 两种拒绝，并将两者视为“设备不支持”，
  据此降级隐藏对应 UI；不得因拒绝而断开连接或视为协议违例。
- **探测**：主机可在停机时以 `MP_CTRL_SET_MODE`（或 `NOW`）探活；`OK`/`LOCKED` 表示
  设备已实现，`BAD_CMD`/`UNSUPPORTED` 表示未实现。

---

## 附录 A：金帧勘误记录（已修正）

`test_vectors.json` 的原始 hex 曾与本文 §1–§6 契约不符，已按 controller
ruling 修正，现行文件与契约一致：

1. **`proto_info_req_seq0` 的 CRC 字节序**：原为 `CD E9`（大端，转录错误），
   已改为 `E9 CD`（`0xCDE9` 的小端）。CRC 覆盖范围与值不变。
2. **`proto_info_resp_sim` 缺少响应状态字节**：原载荷 10 字节，未含
   `mp_status_t`。已补 `status = MP_STATUS_OK`（`00`）为首字节。
3. **`proto_info_resp_sim` 设备名与 sim 实现不一致**：原金帧设备名为 `"SIM"`，
   而 sim 实际发送 `"gui-sim"`。已按 sim 真实输出改为 `name_len = 7`、名称
   `67 75 69 2D 73 69 6D`，`LEN = 0x000F`，CRC 按独立实现重算为 `0x5DCF`
   （线序 `CF 5D`）；`sim_device.rs` 新增编码侧逐字符闭环测试。

修正后 4 个金帧的 CRC 均按小端放置，响应帧均以状态字节开头，与
`motor_proto.h` 及本文 §1–§6 一致；`vectors_valid.rs` 的结构校验继续成立。
