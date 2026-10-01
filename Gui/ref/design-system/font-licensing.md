# 字体授权结论

- 日期：2026-09-30（CRT 字体栈切换同日更新）
- 适用：`Gui/app`（上位机）与固件 USB Terminal
- 结论一句话：**只用 OFL 字体，方正在任何情况下都不得打包。**

---

## 1. 本项目采用（可随产品分发）

**单一归属：`Gui/app/assets/fonts/`** —— 字体随 App 打包，
由 `pubspec.yaml` 注册、`main.dart` 登记进 `LicenseRegistry`。

| 字体 | 用途 | 授权 | 文件（位于 `Gui/app/assets/fonts/`） |
| :--- | :--- | :--- | :--- |
| **VT323** | 拉丁 / 数字 / 单位 | **SIL OFL 1.1** | `VT323-Regular.ttf` |
| **Fusion Pixel 12** | 汉字 正文 / 标题 | **SIL OFL 1.1** | `FusionPixel12-zh_hans.ttf` |
| **Fusion Pixel 10** | 汉字 标签类 | **SIL OFL 1.1** | `FusionPixel10-zh_hans.ttf` |
| **Silkscreen** | 区分 / 增强标签 | **SIL OFL 1.1** | `Silkscreen-Regular.ttf` / `-Bold.ttf` |
| **Share Tech Mono** | 密集等宽（备选） | **SIL OFL 1.1** | `ShareTechMono-Regular.ttf` |

授权原文同目录：`OFL-VT323.txt` / `OFL-FusionPixel.txt` /
`OFL-Silkscreen.txt` / `OFL-ShareTechMono.txt`。

SIL OFL 1.1 允许**嵌入、打包、随产品分发**，包括商业产品，
条件是保留版权声明且不得单独售卖字体本身。

> **合计 10.85 MB**，比原先依赖系统字体的方案更小，且**显示效果不再依赖用户机器装了什么字体**。

### 为何是这两个字族

TNO 模组 `interface/TNO_fonts.gfx` 的首个字体即 `vt323_72_WT_outline`
（DEC VT320 终端体）—— **VT323 是模组实际在用的字体**，故采纳。
VT323 不含汉字，汉字另配同为像素位图的 Fusion Pixel。

详细选型与像素网格约束见 [`design-system.md`](design-system.md) §5。

---

## 2. 明确禁用

参考素材（TNO 模组及其衍生工具）中出现过以下字体，**均不得打包进产品**：

| 字体 | 来源 | 问题 |
| :--- | :--- | :--- |
| **FZRuiZHJW（方正锐体）** | TNO 中文显示体 | **方正商业字体**。方正公司在中国对商用侵权维权积极，有真实法律与财务风险 |
| **FZWH（方正）** | TNO 国家精神标签 | 同上 |
| **huangyouti（黄油体）** | TNO 中文 | 个人使用免费，**商用需单独授权** |
| **Zpix（最像素）** | 中文像素字体候选 | **付费授权** —— README 明示 "Paying for the font grants you the corresponding license"。曾作为 Fusion Pixel 的候选，因授权出局 |

**替代方案**：中文一律改用 **Fusion Pixel**（OFL）。
同为像素位图，与 VT323 气质一致；并已通过字形覆盖校验
（见 §5），确认覆盖工程全部汉字。

---

## 3. 待核实（如未来要用，需先查证）

以下字体出现在参考素材或选型过程中，但**授权状态未确认**，本项目当前不使用：

`OldTypeNr` · `Bombard` · `kenyan coffee` · `Cubic` · `Pure` · `BombardierTNOV6.otf`

> 若要引入，先确认其许可是否允许嵌入与商业分发，并在本文件补录结论。

---

## 4. 处置记录

| 日期 | 事项 |
| :--- | :--- |
| 2026-09-30 | 从 Firefox 参考素材清点出 10 个自托管字体；确认方正系列与黄油体不可用；落地 Electrolize + Aldrich（OFL） |
| 2026-09-30 | **改用 CRT 终端字体栈**：解包 TNO 模组本体，确认其使用 VT323，遂以 VT323 为主字体；汉字配 Fusion Pixel 10/12。Electrolize / Aldrich 退役并从 `assets/fonts/` 移除（`Gui/ref/design-system/fonts/` 副本同批清理，消除重复归属） |
| 2026-09-30 | 字形覆盖校验否掉 **Ark Pixel 12px**（缺 `执辨热警拖滚` 六字）与 **Zpix**（付费授权），最终采用 Fusion Pixel |

---

## 5. 复现

字体不再随 `design-system/` 分发 —— **唯一归属是 `Gui/app/assets/fonts/`**，
由 App 的 `pubspec.yaml` 注册。

来源：

- VT323 —— Google Fonts 官方仓库（OFL）
  `https://raw.githubusercontent.com/google/fonts/main/ofl/vt323/`
- Silkscreen —— Google Fonts 官方仓库（OFL）
  `https://raw.githubusercontent.com/google/fonts/main/ofl/silkscreen/`
- Share Tech Mono —— Google Fonts 官方仓库（OFL）
  `https://raw.githubusercontent.com/google/fonts/main/ofl/sharetechmono/`
- Fusion Pixel —— `TakWolf/fusion-pixel-font` releases（OFL）
  `https://github.com/TakWolf/fusion-pixel-font/releases`

**打包任何中文字体前必跑覆盖校验**：

```powershell
powershell -NoProfile -File ..\tools\check-font-coverage.ps1 `
  -Font <font.ttf> -SourceDir ..\..\app\lib
```
