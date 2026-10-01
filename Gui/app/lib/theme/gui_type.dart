import 'package:flutter/material.dart';

import 'gui_palette.dart';

/// 字体刻度 —— **CRT 终端字体栈**。
///
/// 选型来自 TNO「硅晶之梦」模组本体解包：其 `interface/TNO_fonts.gfx` 的
/// 首个字体即 `vt323_72_WT_outline`（DEC VT320 终端体），故拉丁与数字一律
/// 用 **VT323**。VT323 不含汉字（U+4E00+），汉字另配同为像素位图的
/// **Fusion Pixel**，并已用 `Gui/ref/tools/check-font-coverage.ps1`
/// 校验覆盖工程全部汉字。
///
/// 需要区分 / 增强处用 **Silkscreen**（像素强调体），密集表格可用
/// **Share Tech Mono**（收敛的终端等宽）。四者均为 SIL OFL 1.1，随包分发。
///
/// ## 像素网格（重要）
///
/// Fusion Pixel 是**位图式**字体，只在**原生尺寸及其整数倍**上锐利：
/// 把 12px 压到 10px 等于落在 0.833 倍网格，像素边界对不齐、边缘发虚。
/// 而 Fusion Pixel 只提供 8 / 10 / 12 三档，**没有更大的**。
///
/// 因此中文一律走 [pixelSize]（12，可用的最大原生档）——
/// 原先标签类用 10px 档，实测偏小、可读性不足；中文没有 14/16 原生档，
/// 硬拉大只会发虚，故**统一到 12 并把层级交给颜色与间距**，
/// 这也正是 CRT 终端「同字号、靠亮度分层」的做法。
/// 确需放大的标题用 [pixelSizeLarge]（2 倍整数，仍锐利）。
class GuiTypeScale {
  const GuiTypeScale(this.palette);

  final GuiPalette palette;

  /// 拉丁 / 数字 / 单位 —— VT323（DEC 终端体，TNO 模组同款）。
  static const String displayFamily = 'VT323';

  /// 数值与参数名 —— 同 VT323（等宽数字由 tabular figures 保证）。
  static const String monoFamily = 'VT323';

  /// 汉字 —— Fusion Pixel 12（12px 原生，可用的最大档）。
  static const String zhFamily = 'Fusion Pixel 12';

  /// 区分 / 增强 —— Silkscreen（像素强调体）。
  static const String emphFamily = 'Silkscreen';

  /// 密集等宽 —— Share Tech Mono（比 VT323 收敛，用于窄表格）。
  static const String denseFamily = 'Share Tech Mono';

  /// 中文回退链。VT323 不含汉字，凡以它为主字体、
  /// 内容可能含中文的样式都必须挂上，否则中文会掉到系统字体、脱节。
  static const List<String> zhFallback = <String>[zhFamily];

  /// 中文像素网格 —— 唯一档位 [logical px]。
  static const double pixelSize = 12;

  /// 中文像素网格 —— 2 倍档 [logical px]，仍为整数倍故保持锐利。
  static const double pixelSizeLarge = 24;

  /// Reads the scale for the nearest palette.
  static GuiTypeScale of(BuildContext context) =>
      GuiTypeScale(GuiPalette.of(context));

  /// 顶栏应用标识 —— VT323，全大写宽字距。
  TextStyle get title => TextStyle(
    fontFamily: displayFamily,
    fontFamilyFallback: zhFallback,
    fontSize: 18,
    letterSpacing: 18 * 0.16,
    color: palette.fg,
  );

  /// 小节标题的拉丁副标 —— VT323 全大写，强调色。
  TextStyle get section => TextStyle(
    fontFamily: displayFamily,
    fontFamilyFallback: zhFallback,
    fontSize: 16,
    letterSpacing: 16 * 0.16,
    color: palette.accent,
  );

  /// 标签 / 单位 —— VT323（拉丁）+ Fusion Pixel 12（汉字）。
  ///
  /// 汉字 advance 恰为字号本身（12px 字即 12px 宽/字），比 VT323 的拉丁宽，
  /// 故字距压到 0.04em 给单行布局让位。
  TextStyle get label => TextStyle(
    fontFamily: displayFamily,
    fontFamilyFallback: zhFallback,
    fontSize: pixelSize,
    letterSpacing: pixelSize * 0.04,
    color: palette.fgMute,
  );

  /// 中文小节标题 —— Fusion Pixel 12，落在像素网格上。
  ///
  /// 层级不再靠字号拉开（像素字体放大即失真），改由颜色、字距与
  /// 下方那条亮线承担 —— 这也正是 CRT 终端「同字号、靠亮度分层」的做法。
  TextStyle get sectionTitle => TextStyle(
    fontFamily: zhFamily,
    fontSize: pixelSize,
    letterSpacing: pixelSize * 0.08,
    color: palette.fgStrong,
  );

  /// 中文放大标题 —— 2 倍像素网格，用于确需突出的位置。
  TextStyle get sectionTitleLarge => TextStyle(
    fontFamily: zhFamily,
    fontSize: pixelSizeLarge,
    letterSpacing: pixelSizeLarge * 0.06,
    color: palette.fgStrong,
  );

  /// 大号读数 —— VT323，等宽数字保证刷新时不抖动。
  TextStyle get data => TextStyle(
    fontFamily: monoFamily,
    fontFamilyFallback: zhFallback,
    fontSize: 36,
    letterSpacing: 36 * 0.01,
    color: palette.fgStrong,
    fontFeatures: const [FontFeature.tabularFigures()],
  );

  /// 中文正文 —— Fusion Pixel 12。
  TextStyle get body => TextStyle(
    fontFamily: zhFamily,
    fontSize: pixelSize,
    letterSpacing: pixelSize * 0.04,
    color: palette.fg,
  );

  /// 强调标签 —— Silkscreen，用于需要与常规文本区分的短标签
  /// （告警、状态、模式名）。比 VT323 更重更方，一眼可辨。
  TextStyle get emph => TextStyle(
    fontFamily: emphFamily,
    fontSize: 12,
    letterSpacing: 12 * 0.1,
    color: palette.fgStrong,
  );

  /// 密集等宽 —— Share Tech Mono，用于参数名等需要更高字符密度的场合。
  TextStyle get dense => TextStyle(
    fontFamily: denseFamily,
    fontSize: 12,
    color: palette.fg,
    fontFeatures: const [FontFeature.tabularFigures()],
  );
}
