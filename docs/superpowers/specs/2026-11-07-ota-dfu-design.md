# OTA / DFU 设计（active/backup 双槽 + 极简 bootloader）

> 状态：P0–P4 已实现（2026-11-07），待台架验证。
> 相关代码：`Interface/intf_boot.h`、`App/Platform/Src/boot_meta.c`、
> `App/Platform/Src/boot_mailbox.c`、`bootloader/`、`linkers/gcc/{bootloader,slot_app}.ld`、
> `scripts/gen_image_header.py`。

## 1. 目标与约束

- 现场升级走标准 **DFU 1.1**（`dfu-util`），不引入私有 OTA 协议；
- **active/backup 双槽**：升级写非活动槽，校验通过后切换；应用启动 2s 内确认，连续 3 次
  未确认自动回滚；
- bootloader 极简：只做槽选择/CRC 校验/跳转 + DFU；**不跑 FreeRTOS、不跑电机控制**；
- 校验用 IEEE CRC32（头 + payload）；**不做加密/运行时解密**（性能优先）；
- Windows 免驱：bootloader 自带 MS OS 2.0/WCID，`dfu-util` 直接可用。

## 2. Flash 布局（1MB，HPM53M1）

```text
0x80000000 - 0x80003000   12KB  nor cfg option + ROM boot header
0x80003000 - 0x8001D000  104KB  bootloader（TinyUSB DFU）
0x8001D000 - 0x8001E000    4KB  ACTIVE 槽 镜像日志（追加写，诊断用）
0x8001E000 - 0x8001F000    4KB  BACKUP 槽 镜像日志（追加写，诊断用）
0x8001F000 - 0x80020000    4KB  启动控制日志（active/pending/attempts）
0x80020000 - 0x8008E000  440KB  ACTIVE 槽
0x8008E000 - 0x8008F000    4KB  保留（槽间对齐间隙）
0x8008F000 - 0x800FD000  440KB  BACKUP 槽
0x800FD000 - 0x800FFFFF   12KB  保留区（齿槽表 / 参数 / 自检，两槽共用）
```

- 保留区地址来自 `intf_flash->get_size()` 的绝对计算，**两槽共用同一份**，升级不复制；
- 槽镜像当前约 304KB（含头），余量约 148KB；
- 槽边界 4KB 对齐即可（写入器按 4KB 扇区处理）。

## 3. 镜像格式

槽基址前 64B 为镜像头，入口固定 `槽基址 + 0x40`：

| 偏移 | 字段 | 说明 |
| :--- | :--- | :--- |
| 0x00 | `signature` | `0x0A4D5048`（沿用 `BOARD_UF2_SIGNATURE`） |
| 0x04 | `magic` | `0x47494D47` "GIMG" |
| 0x08 | `header_version` | 1 |
| 0x0A | `slot_id` | 0=A / 1=B |
| 0x0C | `image_version` | 构建号（`git rev-list --count`） |
| 0x10 | `image_size` | payload 字节数（不含头） |
| 0x14 | `image_crc32` | 覆盖 payload |
| 0x18 | `load_addr` | 槽基址 |
| 0x1C | `entry_offset` | 0x40 |
| 0x20 | `header_crc32` | 覆盖 [0x00, 0x20) |
| 0x24 | reserved[7] | 0 |

生成：`scripts/gen_image_header.py` 读取槽链接产物 `demo.bin`（前 64B 为全 0 占位），
写入头并重算 CRC。`slot_id`/`load_addr` 让 bootloader 能拒绝"发错槽"的镜像。
同一份头也会写回输出 `slot_x.elf` 的 `.image_header` 段（`--elf`），因此
**Ozone/J-Link 直接加载 ELF 也能启动**，不需要改用 bin。

## 4. 元数据日志

两类定长记录，均为"顺序追加 + 记录 CRC"，断电最多丢失最后一条未完成记录；
扇区写满后擦除重来。

- 镜像日志（每槽一本，32B/条）：`FLASHING`（DFU 开始写）→ `READY`（manifest 校验通过）。
  **不作为启动判据**（半写镜像的 payload CRC 必然失败；用日志判失效会误伤 J-Link 直烧）；
- 启动控制日志（16B/条）：`seq` 单调 + `active/pending/attempts/confirmed`，取 seq 最大有效记录。

## 5. 启动流程

```text
main:
  board_init → intf_clock_init → boot_init
  邮箱（GPR12/13 = "G661"/"DFU0"）？
    ├─ 是 → dfu_run(目标槽：active 有效则 active ^ 1，否则 active)
    └─ 否 → boot_decide:
         pending != NONE?
           ├─ 校验失败 → 清 pending，回滚 active
           ├─ attempts+1 > 3 → 清 pending，回滚 active
           └─ 校验通过 → attempts+1，跳 pending
         active 有效 → 跳 active
         active 无效 → 另一槽有效则改写 active 并跳转
         两槽都无效 → 进 DFU
```

跳转：`tud_deinit`（若本次跑过 DFU）→ 清目标槽范围 I-cache → `fence.i` →
关全局中断 → `跳 槽基址 + 0x40`。应用的 `start.S` 重置 `mstatus`、使能 cache、
初始化 `.data/.bss`。

**I-cache 必须清**：同一上电周期内 A→B→A 连续升级时，目标槽可能残留旧镜像的
指令行；只靠 `fence.i` 不保证失效。

## 6. DFU 流程

1. 应用：`dfu confirm`（终端）或 `CTRL_ENTER_DFU`（协议）→ 检查 FOC OFF/无停机挂起 →
   写邮箱 → **hot reset**（冷复位会清 SYSCTL GPR，邮箱丢失）；
2. bootloader：消费邮箱 → 选目标槽：`active` 有效 → 另一槽；`active` 无效
   （只烧了 bootloader）→ `active` 本身（首次安装落到 A）。枚举 `34B7:6619`，
   接口名显示 `G6618 DFU (ACTIVE)` / `(BACKUP)`，`dfu-util -l` 可直接看出该发哪个 bin；
3. `dfu-util -a 0 -D slot_backup.bin`：TinyUSB DFU 1.1，块 0 起顺序写入，
   4KB 扇区缓冲，擦/写期间关全局中断；
4. manifest：读回镜像头 → 槽号/长度/CRC 校验 → 追加 `READY` → 启动控制写
   `pending=目标槽, attempts=0` → 1.2s 后软复位；
5. 复位后 bootloader 启动 pending；应用 2s 内确认 `active=pending, pending=NONE`；
6. 连续 3 次未确认 → 回滚 active。

失败语义：块乱序/越界/擦写失败 → `DFU_STATUS_ERR_*`，会话中止，**旧镜像不受影响**；
发错槽的文件在首块即被拒绝（`ERR_FIRMWARE`）。

DFU 空闲 60s（且存在可回退的 active）→ 自动复位回应用。

## 7. 构建与烧录

```bash
make bootloader     # output/bootloader.bin（0x8000400 起）
make slot-active         # output/slot_active.bin（0x80020000）
make slot-backup         # output/slot_backup.bin（0x8008F000）
make ota-package    # output/ota/{bootloader,slot_active,slot_backup,manifest.json}
make flash-ota      # J-Link 整片擦除 + bootloader + ACTIVE 槽
make build          # 旧的整体镜像（0x80003000，ROM 直接启动），过渡期保留
```

槽构建差异：`-DAPP_SLOT_ID=0|1` → 编译定义 `APP_SLOT_ID` +
链接符号 `_slot_base/_slot_size` + `linkers/gcc/slot_app.ld`（丢弃
`.nor_cfg_option/.boot_header`，`.image_header` 占 0x40）。

**烧录纪律**：`make build` 只更新 `build/output/demo.*`；`output/*.bin` 只有
`make artifacts`/`make slot-active` 等目标才更新。`flash-ota` 会清空参数区与齿槽表。

## 8. 应用侧改动

- `App/Platform/Src/app_boot.c`：槽状态查询、启动确认、进入 DFU；
- `App/Logic/app_logic.c`：`app_io_step` 第 2000 拍（约 2s）在任务末尾调用一次
  `app_boot_confirm()`（与参数写入同一任务上下文，避免并发进入 flash 驱动）；
- 终端 `dfu [show|confirm|slot active|backup]`（`slot` 强制下次启动槽，台架 active/backup 切换用）；
- 协议 `MP_CMD_BOOT_INFO = 0x0E`、`MP_CTRL_ENTER_DFU = 0x0B`、`CAP_DFU`（v1.6）。

## 9. 台架验证清单

1. `make flash-ota` → 上电：bootloader 启动 ACTIVE 槽，`dfu show` 显示 `running=ACTIVE boot=ACTIVE`；
2. 终端 `dfu confirm`：设备枚举为 `34B7:6619`，`dfu-util -l` 可见；
3. `dfu-util -a 0 -D output/ota/slot_backup.bin` → 复位后 `running=BACKUP boot=BACKUP`；
4. 反向：`dfu confirm` → 下载 `slot_active.bin` → 回到 ACTIVE；
5. 坏镜像：改一个字节再下载 → manifest 拒绝，设备仍跑旧槽；
6. 回滚：下载一个"启动即挂起"的镜像（或用调试器在 `main` 打断点/不确认）→
   3 次后自动回滚到旧槽；
7. 断电注入：DFU 传输中拔电 → 重新上电仍启动旧槽；
8. 参数/齿槽表在多次升级后保持不变（保留区未被触碰）。

## 10. 已知限制

- 槽镜像的版本号只做展示，**不做防回退**（降级是允许的）；
- 连续角/多圈位置在升级复位后丢失（与普通复位一致，需要 homing）；
- DFU 期间无电机保护（bootloader 不跑控制），必须 FOC OFF 才允许进入；
- `flash-ota` 是"首次迁移"工具，会清空参数与齿槽表；
- Windows 第 9 条：PID 变化会改变 COM 口号（应用 PID 0x6618 / DFU PID 0x6619）。
