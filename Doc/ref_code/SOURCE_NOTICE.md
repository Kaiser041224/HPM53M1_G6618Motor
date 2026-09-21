# 出处、完整性与许可证

## 1. 上游信息

| 项 | 值 |
| :--- | :--- |
| 仓库 | YHorizon-JM（开源一体化关节电机） |
| 本地路径 | `D:\Codes\GitHub\YHorizon-JM` |
| 提交 | `800313139bd607c36b6641574cba9738a1f34f90` |
| 提交说明 | 更新 README 文件，包含 CNC 订单备注和新图片；添加了关于 PCB 组和 V2 发布的信息。 |
| 复制日期 | 2026-09-21 |
| 上游目录 | `firmware/Foc/**`、`firmware/App/{config.h,servo.c,servo.h,cali.c,cali.h,cali_nv.c,cali_nv.h}` |

上游仓库中与本项目相关的许可范围：**`firmware/`、`sdk/`、`tools/` 采用 GPLv3**；
`mechanical/`、`hardware/` 采用 CC BY-NC-SA 4.0（本副本未取用后两者）。

## 2. 复制方式与完整性校验

**逐字复制，零修改**——未改动任何字节，未添加版权头，未调整缩进。
复制后对源文件与副本逐个做 SHA256 比对，17/17 一致。

可用以下命令复核（副本侧）：

```powershell
Get-ChildItem -Recurse -File .\reference_src |
  ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)  $($_.Name)" }
```

| SHA256 | 文件 |
| :--- | :--- |
| `90DB81B8EAA122702495973F8D5D9D877C3D2A0BD71EE0262A4FB7005B1D9381` | `Foc/foc_math.c` |
| `5151DACDD9A61338001828C30B2327193587D069A7EA4C441232594DA427B1E8` | `Foc/foc_math.h` |
| `BB997B99BD94AA5A14A245FA0AF8CF8DF4EA7718058559A5BA526A94414D9155` | `Foc/foc_svpwm.c` |
| `DE5B839EF27B13F54B5064F9D3F091C9140654E0E135BFB9435931F30895970F` | `Foc/foc_svpwm.h` |
| `834024BB6585C4515DEA8133F2E524118DDA0238D86C16E805562021FC70A19D` | `Foc/foc_encoder.c` |
| `7C15C948547DB9701832B6B59CDE170E0F9460A74F20A00B762B50C69D8588AE` | `Foc/foc_encoder.h` |
| `ED30F1F95607D4B1528289673991174B56A4FCB403264269573606798322BEEF` | `Foc/foc_vernier.c` |
| `671333E3A9764A41820015E577E5ABFA5EAB58EDA200A14A48ACD014622AC458` | `Foc/foc_vernier.h` |
| `FFD0358D590DE788C300EBC96D16DB1AA851D6B5B14A426351E95546C30BA2BC` | `Foc/foc_current.c` |
| `7B992B03CBD48D598F29BAC72E3380CDE48F07FCA8D8D88608210EF8354AD936` | `Foc/foc_current.h` |
| `8F187C6742F36F88F1A00FD5E35A53BE7B7974CAE69F974836B6084B6367A372` | `App/config.h` |
| `BCA618FF2AF1C371F8867C7994EF52FC89AC33D577B19DE43C247E4269C63EC5` | `App/servo.c` |
| `9E155CBE643A046F61B973D409BC87C74A4CAD7C0F2192BE70C71AB003FC16B0` | `App/servo.h` |
| `67FD0BBB4FC84A6AC9537756971EAFC0F6CB740CEA232D2264397D05F6E53947` | `App/cali.c` |
| `7C92E6B3389E0E2C7B0EBCD28FD0A7B306944A69C19D9BD0FE0E7D78D257E988` | `App/cali.h` |
| `975C23003E0A5407A4373C1B36B2A5B731453E9E008682FB2F6D0479751E9731` | `App/cali_nv.c` |
| `33CD21AA37C2D9343B07DC1C4CCF290C015E598EC7CD9AA613D03190E05B94A3` | `App/cali_nv.h` |
| `230184F60BAE2FEAF244F10A8BAC053C8FF33A183BCC365B4D8B876D2B7F4809` | `LICENSE-GPL-3.0.txt`（上游 `LICENSES/GPL-3.0.txt` 副本） |

## 3. 许可证义务

`reference_src/` 全部文件以及 `LICENSE-GPL-3.0.txt` 均为 **GPLv3**（上游 `firmware/` 的许可）。
因此：

1. **保持许可证与版权信息完整**：`LICENSE-GPL-3.0.txt` 不得删除或修改；本目录内的文件不得
   抹去上游归属。
2. **不得把 GPLv3 代码编入 BSD-3-Clause 构建。** 本工程固件按 `AGENTS.md` 采用 BSD-3-Clause；
   一旦 GPLv3 代码与本工程目标文件链接进同一产物，整个产物须按 GPLv3 分发。
   `Doc/ref_code/` 因此**刻意游离于构建之外**（`CMakeLists.txt` 未引用 `Doc/`，
   `App/` 的 include 路径也不含本目录）。**请勿添加任何引用它的构建文件。**
3. **要把某个算法真正用进本工程，必须重写实现（clean-room）**：只借鉴思路与数学公式，
   不复制代码文本、命名与注释。参考 `docs/superpowers/specs/` 里既有的做法——本工程的
   `foc_math` / `foc_current` / `foc_modulation` / `foc_angle` 已是独立实现，
   与上游的关系是"方法相同、代码无关"。
4. **`host_test/` 亦按 GPLv3 分发**：脚本本身是本工程新写的，但它 `#include` 并链接
   `reference_src/Foc/*.c`，构成衍生作品，故不套用本工程的 `Alliance HardwareGroup` /
   BSD-3-Clause 版权头。

**关于上游"禁止商用"的说明（重要）**：上游 `LICENSE` 在 GPLv3 之上附加了
"Commercial use by others is not permitted"。GPLv3 §7 明确规定：若收到的程序带有此类
"进一步限制"，接收者**可以删除该条款**；且 GPLv3 本身授予商用权利。因此该附加限制
对上游 `firmware/` 部分**在法律上站不住**。这意味着：

- 不能把"上游禁止商用"当作约束本工程的条件——真正的约束是 **GPLv3 的传染性**；
- 反过来，本工程也不应基于本副本主张任何排他权利。

## 4. 上游未提供、本工程需自行注意的事项

- 上游自有源文件（含本副本 17 个文件）**没有任何版权头 / SPDX 标识**。本副作为保持原样未补，
  不要误以为这些文件处于公有领域。
- 上游 `LICENSES/` 只收录 CC BY-NC-SA 4.0 与 GPLv3 全文；其捆绑的第三方代码
  （GD32 SPL = BSD-3-Clause、CMSIS = Apache-2.0、SEGGER RTT = SEGGER 自有 BSD-1 类）
  的全文仅在各文件头，未集中收录。**本副本未取用这些第三方文件。**
- 上游仓库仅 2 个提交、历史被压缩，`CHANGELOG.md` 为空，因此**无法追溯**其 NV 记录
  旧版本（v1/v2）的原始布局等决策依据。若依赖 `cali_nv.c` 的迁移逻辑，需自行在实机上验证。
