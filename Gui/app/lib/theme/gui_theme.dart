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
  final scheme = ColorScheme.fromSeed(
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
  return ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    scaffoldBackgroundColor: palette.bgBase,
    fontFamily: GuiTypeScale.zhFamily,
    extensions: const <ThemeExtension<dynamic>>[palette],
    cardTheme: CardThemeData(shape: square, color: palette.bgPanel, elevation: 0),
    dividerTheme: DividerThemeData(
      color: palette.line,
      thickness: palette.lineWidth,
      space: palette.lineWidth,
    ),
    dialogTheme: DialogThemeData(shape: square, backgroundColor: palette.bgPanel),
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
    inputDecorationTheme: const InputDecorationTheme(
      border: OutlineInputBorder(borderRadius: BorderRadius.zero),
    ),
  );
}
