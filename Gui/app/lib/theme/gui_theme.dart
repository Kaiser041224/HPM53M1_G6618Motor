import 'package:flutter/material.dart';

import 'gui_palette.dart';
import 'gui_type.dart';

export 'gui_glow.dart';
export 'gui_palette.dart';
export 'gui_type.dart';

/// Assembles the TNO 硅晶之梦 [ThemeData].
///
/// Scaffold sits on [GuiPalette.bgBase], corners are square (design-system
/// §3.1), and the [GuiPalette] itself is published as a theme extension so
/// widgets can read semantic tokens via `GuiPalette.of(context)`.
ThemeData buildGuiTheme() {
  const palette = GuiPalette();
  final scheme =
      ColorScheme.fromSeed(
        seedColor: palette.accent,
        brightness: Brightness.dark,
      ).copyWith(
        surface: palette.bgBase,
        onSurface: palette.fg,
        primary: palette.accent,
        onPrimary: palette.bgVoid,
        secondary: palette.accent2,
        onSecondary: palette.bgVoid,
        error: palette.stFault,
        onError: palette.bgVoid,
        outline: palette.line,
        surfaceContainerHighest: palette.bgRaise,
      );
  const square = RoundedRectangleBorder(borderRadius: BorderRadius.zero);
  const scale = GuiTypeScale(palette);
  return ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    scaffoldBackgroundColor: palette.bgBase,
    fontFamily: GuiTypeScale.zhFamily,
    extensions: const <ThemeExtension<dynamic>>[palette],
    // 交互态一律落到 token 青：禁用 Material 默认的白色 focus / hover 层，
    // 它们叠在深底上会显出灰白偏黄的"残影"。光标同色（见 textSelectionTheme）。
    focusColor: palette.accent.withValues(alpha: 0.12),
    hoverColor: palette.accent.withValues(alpha: 0.06),
    splashColor: palette.accent.withValues(alpha: 0.10),
    highlightColor: palette.accent.withValues(alpha: 0.08),
    textSelectionTheme: TextSelectionThemeData(
      cursorColor: palette.accent,
      selectionColor: palette.accent.withValues(alpha: 0.40),
      selectionHandleColor: palette.accent,
    ),
    cardTheme: CardThemeData(
      shape: square,
      color: palette.bgPanel,
      elevation: 0,
    ),
    dividerTheme: DividerThemeData(
      color: palette.line,
      thickness: palette.lineWidth,
      space: palette.lineWidth,
    ),
    // 弹窗统一：深底 bgBase + 青色 1.5px 直角外框 + 弱辉光（glowSoft 级）；
    // 标题 / 内容 fgStrong。按钮行由各对话框自行使用 GuiButton 三级。
    dialogTheme: DialogThemeData(
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.zero,
        side: BorderSide(color: palette.accent, width: 1.5),
      ),
      backgroundColor: palette.bgBase,
      elevation: 8,
      shadowColor: palette.accent.withValues(alpha: palette.glowSoft.a),
      titleTextStyle: scale.sectionTitle.copyWith(color: palette.fgStrong),
      contentTextStyle: scale.body.copyWith(color: palette.fgStrong),
    ),
    bottomSheetTheme: BottomSheetThemeData(
      shape: square,
      backgroundColor: palette.bgPanel,
    ),
    elevatedButtonTheme: ElevatedButtonThemeData(
      style: ElevatedButton.styleFrom(shape: square),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(shape: square),
    ),
    textButtonTheme: TextButtonThemeData(
      style: TextButton.styleFrom(shape: square),
    ),
    // 输入框全局风格（v5）：整框 OutlineInputBorder —— 青 accent 1px、直角、
    // 深填充 bgRaise 级，聚焦亮青 1.5px（辉光由 GuiTextField 提供）。
    // 显式铺满全部状态边框，杜绝 Material 默认下划线（琥珀/双线）从任何
    // 状态渗入；label / hint fgDim，错误 stFault。
    inputDecorationTheme: InputDecorationThemeData(
      filled: true,
      fillColor: palette.bgRaise,
      isDense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      border: _guiOutline(palette.accent),
      enabledBorder: _guiOutline(palette.accent),
      focusedBorder: _guiOutline(palette.lineHi, width: 1.5),
      errorBorder: _guiOutline(palette.stFault),
      focusedErrorBorder: _guiOutline(palette.stFault, width: 1.5),
      disabledBorder: _guiOutline(palette.lineDim),
      labelStyle: scale.label.copyWith(color: palette.fgDim),
      floatingLabelStyle: scale.label.copyWith(color: palette.accent),
      hintStyle: scale.label.copyWith(color: palette.fgDim),
      errorStyle: scale.label.copyWith(color: palette.stFault),
      prefixIconColor: palette.fgDim,
      suffixIconColor: palette.fgDim,
      iconColor: palette.fgDim,
    ),
    // 全站 Tooltip 统一语汇：暗底（bgPanel）+ 青色 1px 锐利边框（accent）、
    // 直角、8×10 内边距、fgStrong 文字，阴影压暗。取代 Material 默认的
    // 浅色圆角提示 —— 模式按钮提示 / 徽标 / 参数 tooltip 全部继承。
    tooltipTheme: TooltipThemeData(
      decoration: BoxDecoration(
        color: palette.bgPanel,
        border: Border.all(color: palette.accent, width: palette.lineWidth),
        borderRadius: BorderRadius.zero,
        boxShadow: <BoxShadow>[
          BoxShadow(
            color: palette.bgVoid.withValues(alpha: 0.72),
            blurRadius: 8,
            offset: const Offset(0, 2),
          ),
        ],
      ),
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      margin: EdgeInsets.zero,
      textStyle: TextStyle(
        fontFamily: GuiTypeScale.zhFamily,
        fontFamilyFallback: const <String>[GuiTypeScale.emphFamily],
        fontSize: GuiTypeScale.pixelSize,
        letterSpacing: GuiTypeScale.pixelSize * 0.04,
        color: palette.fgStrong,
      ),
    ),
  );
}

/// Square [OutlineInputBorder] with an explicit TNO [color] / [width].
///
/// Used to fill every [InputDecorationThemeData] state so no Material default
/// (amber underline / double focus line) can leak through an unspecified state.
OutlineInputBorder _guiOutline(Color color, {double width = 1}) =>
    OutlineInputBorder(
      borderRadius: BorderRadius.zero,
      borderSide: BorderSide(color: color, width: width),
    );
