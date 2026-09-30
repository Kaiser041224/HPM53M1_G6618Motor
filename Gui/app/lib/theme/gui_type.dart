import 'package:flutter/material.dart';

import 'gui_palette.dart';

/// Typography scale from `tokens.css` (`--fs-*` / `--ls-*`).
///
/// Titles are uppercase with wide tracking (hard requirement); numbers are
/// monospace + tabular so their width never changes while refreshing.
class GuiTypeScale {
  const GuiTypeScale(this.palette);

  final GuiPalette palette;

  /// Display face for titles, navigation and buttons (SIL OFL).
  static const String displayFamily = 'Electrolize';

  /// Alternate display face (SIL OFL).
  static const String altFamily = 'Aldrich';

  /// Monospace face for values and parameter names.
  static const String monoFamily = 'Cascadia Mono';

  /// Monospace fallbacks.
  static const List<String> monoFallback = <String>['Consolas'];

  /// Chinese face (SIL OFL).
  static const String zhFamily = 'Noto Sans SC';

  /// Chinese fallbacks.
  static const List<String> zhFallback = <String>['Microsoft YaHei UI'];

  /// Reads the scale for the nearest palette.
  static GuiTypeScale of(BuildContext context) =>
      GuiTypeScale(GuiPalette.of(context));

  /// 15px display title, 0.16em tracking.
  TextStyle get title => TextStyle(
    fontFamily: displayFamily,
    fontSize: 15,
    letterSpacing: 15 * 0.16,
    color: palette.fg,
  );

  /// 12.5px section heading (`.gui-section-title`), accent colored.
  TextStyle get section => TextStyle(
    fontFamily: displayFamily,
    fontSize: 12.5,
    letterSpacing: 12.5 * 0.16,
    color: palette.accent,
  );

  /// 9px label, 0.14em tracking, almost unlit.
  TextStyle get label => TextStyle(
    fontFamily: displayFamily,
    fontSize: 9,
    letterSpacing: 9 * 0.14,
    color: palette.fgMute,
  );

  /// 26px tabular value (`.gui-value`).
  TextStyle get data => TextStyle(
    fontFamily: monoFamily,
    fontFamilyFallback: monoFallback,
    fontSize: 26,
    letterSpacing: 26 * 0.02,
    color: palette.fgStrong,
    fontFeatures: const [FontFeature.tabularFigures()],
  );

  /// 13px Chinese body copy.
  TextStyle get body => TextStyle(
    fontFamily: zhFamily,
    fontFamilyFallback: zhFallback,
    fontSize: 13,
    color: palette.fg,
  );
}
