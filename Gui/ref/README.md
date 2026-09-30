# Gui 视觉参考与设计资产

本目录是 Gui 上位机的**视觉设计唯一来源**：参考截图、实测设计系统、规范模板、渲染预览、复现工具。

**风格定位：TNO 硅晶之梦 · 复古未来主义（Retro-futurism）· 合成器浪潮（Synthwave）**

---

## 目录结构

```text
Gui/ref/
├── README.md                      本文件（索引）
├── screenshots/                   原始参考截图（只读，勿改）
│   ├── TNO面板.jpg                    1920x1080  主面板
│   ├── TNO经济系统界面.jpg             1920x1080  经济界面（图表语汇来源）
│   ├── TNO产品研发决议.png             1127x497   决议窗
│   └── tno科技树武器详情.png           539x677    科技树详情
├── design-system/                 实测得出的设计系统
│   ├── design-system.md              完整规范：色板/结构母题/布局/字体/状态语义
│   ├── font-licensing.md             字体授权结论（含禁用清单）
│   ├── tokens.css                    可直接引用的 CSS 变量 + 可复用构件
│   └── fonts/                        随附字体（OFL，可合法打包）
├── templates/
│   └── gui-main.html              规范模板（单页平铺 · 唯一一份）
├── previews/
│   └── gui-main.png               模板渲染结果
└── tools/
    ├── measure-colors.ps1         色板测量（design-system.md 全部色值的来源）
    └── extract-device-art.ps1     设备 ASCII 艺术字提取（字形 + 色序 + HTML 片段）
```

---

## 怎么用

| 目的 | 看哪个 |
| :--- | :--- |
| 快速看效果 | `previews/gui-main.png` |
| 看完整可交互版 | 浏览器打开 `templates/gui-main.html` |
| 写代码时查色值 / 规则 | `design-system/design-system.md` |
| 接到 Flutter 主题 | `design-system/tokens.css`（转成 ThemeExtension） |
| 确认某字体能否打包 | `design-system/font-licensing.md` |
| 核对某色值怎么来的 | 跑 `tools/measure-colors.ps1` |
| 核对设备标识字形 | 跑 `tools/extract-device-art.ps1` |

---

## 三条不可让步的规则

1. **色值与字形全部来自实测，不靠目测或手抄。**
   测量已推翻两处常见印象：面板角是**直角**（非切角）；次强调色是**黄 `#C8C000`**（非热粉）。
   手抄设备艺术字已出过两次错（色号 213 写成 212、漏前导空格），故固化为脚本。

2. **颜色不得单独承载语义。**
   状态一律「色 + 图标 + 文字」三重编码 —— 需应对色觉差异、灰度截图、投影仪。

3. **`0` 与 `—` 必须区分。**
   轮询失败时不得继续显示旧值冒充实时；每个实时值携带 `live / stale / unknown` 三态。

---

## 复现工具

```powershell
powershell -NoProfile -File tools\measure-colors.ps1      # 重量色板
powershell -NoProfile -File tools\extract-device-art.ps1  # 重提设备艺术字
```

> **两个脚本都必须保留 UTF-8 BOM。**
> Windows PowerShell 5.1 会把无 BOM 的 UTF-8 脚本按系统 ANSI 码页（本机为 GBK）解码，
> 导致中文注释与块字符损坏、脚本报错。用编辑器重存时请确认编码为「UTF-8 with BOM」。

---

## 已知边界

- **不改** `Gui/core`、`Gui/frb`、`Gui/sim` 与协议契约 —— 本目录只影响显示层。
- **字体**：`design-system/fonts/` 内两个字体均为 SIL OFL 1.1，可随产品分发、不限平台。
  TNO 原用的方正系列为**商业字体，不可打包**，已替换为 Noto Sans SC。
- **参考截图为第三方版权内容**，仅用于观感分析与比例测量，**不得**用于产品分发。
- 本目录是**设计资产**，不是运行时资源。Flutter 侧需将 tokens 转为 `ThemeExtension` 后使用，
  不应直接引用本目录文件。

---

## 变更记录

| 日期 | 变更 |
| :--- | :--- |
| 2026-09-30 | 建立。确定 TNO 硅晶之梦方向；色板/结构/字体全部实测 |
| 2026-09-30 | v2：文字改半透明 + 四层轮廓辉光；板块改无边框 + 一亮两暗分割线；连接选项卡移至左上角；设备型号改用终端同款 ASCII 艺术字 |
| 2026-09-30 | 定稿：清理被取代的中间产物，模板与预览统一命名为 `gui-main`，补齐两个复现脚本与授权文档 |
