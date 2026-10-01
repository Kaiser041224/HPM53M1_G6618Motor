# Gui 视觉参考与设计资产

本目录是 Gui 上位机的**视觉设计依据**：参考截图、设计规范、复现工具。

> ### ★ 视觉的唯一事实源是 App 本身
>
> 自 2026-09-30 起，**「界面长什么样」以 `Gui/app` 的运行结果为准**。
> v4 时期的旧 HTML 规范模板与渲染预览已退役（停留在旧字体方案
> Electrolize / Cascadia / Noto，会与实现漂移）。
>
> **现行活样张**只有一份：[`design-system/previews/gui-style-sample.html`](design-system/previews/gui-style-sample.html)
> —— v5 发光管定稿的单一参考样张，随规范同步维护、可双击打开。
> 它表达的是**规则**（六色分工 / 框线分工 / 参数专色 / 字阶 / VFD 条构造），
> 运行结果仍以 App 为准。
>
> 本目录负责的是**为什么这么设计**（依据、实测、约束、授权）。

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
├── design-system/
│   ├── design-system.md              完整规范：色板/结构母题/布局/字体/状态语义
│   │                                 §2.6 = v5 发光色系（现行：六色/框线分工/参数专色/字阶/VFD 条）
│   │                                 §9 = 模组解包美术资源（权威色板、图表手法、字体来源）
│   ├── font-licensing.md             字体授权结论（含禁用清单）
│   ├── tokens.css                    CSS 变量 + 可复用构件（v5 段为注释性，以 App 为准）
│   └── previews/
│       └── gui-style-sample.html     ★ 现行活样张（v5 定稿，可直接双击打开）
└── tools/
    ├── measure-colors.ps1            色板测量（早期色值的来源）
    ├── extract-device-art.ps1        设备 ASCII 艺术字提取（字形 + 色序 + HTML 片段）
    ├── check-font-coverage.ps1       字形覆盖校验（打包中文字体前必跑）
    └── sync-tokens-from-app.ps1      以 App 为准回写 tokens.css 色板段
```

**字体不在本目录** —— 唯一归属是 `Gui/app/assets/fonts/`（随 App 打包）。

---

## 怎么用

| 目的 | 看哪个 |
| :--- | :--- |
| **看界面实际长什么样** | **跑 `Gui/app`**（或看交付的 exe） |
| **快速查看 v5 视觉样张** | `design-system/previews/gui-style-sample.html`（双击打开） |
| 写代码时查色值 / 规则 | `design-system/design-system.md`（现行色板见 §2.6） |
| 拿到当前色值 | `design-system/tokens.css`（**以 App 为准，色板段由 App 生成，勿手改**） |
| 确认某字体能否打包 | `design-system/font-licensing.md` |
| 核对某色值怎么来的 | 跑 `tools/measure-colors.ps1` |
| 核对设备标识字形 | 跑 `tools/extract-device-art.ps1` |
| 打包新中文字体前 | 跑 `tools/check-font-coverage.ps1` |
| 改了 App 配色后 | 跑 `tools/sync-tokens-from-app.ps1` |

---

## 三条不可让步的规则

1. **色值与字形全部来自实测，不靠目测或手抄。**
   测量已推翻两处常见印象：面板角是**直角**（非切角）；次强调色是**黄**（非热粉）。
   手抄设备艺术字已出过两次错（色号 213 写成 212、漏前导空格），故固化为脚本。

2. **颜色不得单独承载语义。**
   状态一律「色 + 图标 + 文字」三重编码 —— 需应对色觉差异、灰度截图、投影仪。

3. **`0` 与 `—` 必须区分。**
   轮询失败时不得继续显示旧值冒充实时；每个实时值携带 `live / stale / unknown` 三态。

---

## 复现工具

```powershell
powershell -NoProfile -File tools\measure-colors.ps1              # 重量色板
powershell -NoProfile -File tools\extract-device-art.ps1          # 重提设备艺术字
powershell -NoProfile -File tools\check-font-coverage.ps1 `       # 字形覆盖校验
  -Font <font.ttf> -SourceDir ..\..\app\lib
powershell -NoProfile -File tools\sync-tokens-from-app.ps1        # App → tokens.css
```

> **所有脚本都必须保留 UTF-8 BOM。**
> Windows PowerShell 5.1 会把无 BOM 的 UTF-8 脚本按系统 ANSI 码页（本机为 GBK）解码，
> 导致中文注释与**块字符**损坏、脚本报错。用编辑器重存时请确认编码为「UTF-8 with BOM」。

---

## 已知边界

- **不改** `Gui/core`、`Gui/frb`、`Gui/sim` 与协议契约 —— 本目录只影响显示层。
- **字体**：全部 SIL OFL 1.1，可随产品分发、不限平台，唯一归属
  `Gui/app/assets/fonts/`。TNO 原用的方正系列为**商业字体，不可打包**，
  已替换为同为像素位图的 Fusion Pixel（OFL）。
- **参考截图为第三方版权内容**，仅用于观感分析与比例测量，**不得**用于产品分发。
  模组解包资源的版权边界见 `design-system.md` §9。
- 本目录是**设计依据**，不是运行时资源。Flutter 侧通过
  `lib/theme/gui_palette.dart` 等 ThemeExtension 使用，不直接引用本目录文件。

---

## 变更记录

| 日期 | 变更 |
| :--- | :--- |
| 2026-09-30 | 建立。确定 TNO 硅晶之梦方向；色板/结构/字体全部实测 |
| 2026-09-30 | v2：文字改半透明 + 四层轮廓辉光；板块改无边框 + 一亮两暗分割线；连接选项卡移至左上角；设备型号改用终端同款 ASCII 艺术字 |
| 2026-09-30 | 定稿：清理被取代的中间产物，模板与预览统一命名为 `gui-main`，补齐两个复现脚本与授权文档 |
| 2026-09-30 | **转为「以 App 为准」**：色板由 `sync-tokens-from-app.ps1` 从 App 生成；字体改 CRT 终端栈（VT323 + Fusion Pixel）并移入 `Gui/app/assets/fonts/`；**退役** HTML 模板、渲染预览、`design-system/fonts/`（消除重复归属） |
| 2026-10-01 | **v5 发光管定稿**：六色光谱 + 框线分工 + 参数专色（量纲派生）+ 大字号 + VFD 条构造，写入 `design-system.md` §2.6；`tokens.css` 增补 v5 注释性 token；新增单一活样张 `design-system/previews/gui-style-sample.html` |
