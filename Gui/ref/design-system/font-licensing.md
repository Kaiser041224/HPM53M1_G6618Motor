# 字体授权结论

- 日期：2026-09-30
- 适用：`Gui/app`（上位机）与固件 USB Terminal
- 结论一句话：**只用 OFL 字体，方正在任何情况下都不得打包。**

---

## 1. 本项目采用（可随产品分发）

| 字体 | 用途 | 授权 | 文件 |
| :--- | :--- | :--- | :--- |
| **Electrolize** | 标题 / 导航 / 按钮 | **SIL OFL 1.1** | `fonts/Electrolize-Regular.ttf` |
| **Aldrich** | 备选标题 | **SIL OFL 1.1** | `fonts/Aldrich-Regular.ttf` |
| **Cascadia Mono** | 数值 / 参数名 | **SIL OFL 1.1** | Windows 内置 |
| **Noto Sans SC** | 中文正文 | **SIL OFL 1.1** | 系统已装 |
| **Noto Serif SC** | 中文描述文字 | **SIL OFL 1.1** | 系统已装 |

授权原文随字体一并存于 `fonts/OFL-*.txt`（Electrolize / Aldrich）。
SIL OFL 1.1 允许**嵌入、打包、随产品分发**，包括商业产品，条件是保留版权声明且不得单独售卖字体本身。

---

## 2. 明确禁用

参考素材（TNO 模组及其衍生工具）中出现过以下字体，**均不得打包进产品**：

| 字体 | 来源 | 问题 |
| :--- | :--- | :--- |
| **FZRuiZHJW（方正锐体）** | TNO 中文显示体 | **方正商业字体**。方正公司在中国对商用侵权维权积极，有真实法律与财务风险 |
| **FZWH（方正）** | TNO 国家精神标签 | 同上 |
| **huangyouti（黄油体）** | TNO 中文 | 个人使用免费，**商用需单独授权** |

**替代方案**：中文一律改用 **Noto Sans SC**。
观感损失很小 —— 本来就是同一「黑体」逻辑，且 Noto 的 Light/DemiLight 字重更适合本项目的技术感需求。

---

## 3. 待核实（如未来要用，需先查证）

以下字体出现在参考素材中，但**授权状态未确认**，本项目当前不使用：

`OldTypeNr` · `Bombard` · `kenyan coffee` · `Cubic` · `Pure`

> 若要引入，先确认其许可是否允许嵌入与商业分发，并在本文件补录结论。

---

## 4. 处置记录

| 日期 | 事项 |
| :--- | :--- |
| 2026-09-30 | 从 Firefox 参考素材清点出 10 个自托管字体；确认方正系列与黄油体不可用；落地 Electrolize + Aldrich（OFL）替代 TNO 的展示体 |

---

## 5. 复现

字体文件随 `design-system/fonts/` 分发；授权原文同目录。
Electrolize / Aldrich 来自 Google Fonts 官方仓库（OFL 目录）：

- `https://raw.githubusercontent.com/google/fonts/main/ofl/electrolize/`
- `https://raw.githubusercontent.com/google/fonts/main/ofl/aldrich/`
