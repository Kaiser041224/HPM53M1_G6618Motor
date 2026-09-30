import 'package:flutter/material.dart';

/// TNO 硅晶之梦 palette.
///
/// Every value is copied verbatim from `Gui/ref/design-system/tokens.css`
/// (pixel-measured, not eyeballed). Nothing here may be invented: page code
/// must read semantic tokens through [GuiPalette.of] instead of hard-coding.
@immutable
class GuiPalette extends ThemeExtension<GuiPalette> {
  const GuiPalette({
    this.bgVoid = const Color(0xFF04080C),
    this.bgBase = const Color(0xFF081018),
    this.bgPanel = const Color(0xFF0A161A),
    this.bgRaise = const Color(0xFF102024),
    this.bgHover = const Color(0xFF162C30),
    this.bgActive = const Color(0xFF1C383C),
    this.lineHi = const Color(0xFF58C8C0),
    this.line = const Color(0xFF1E5C60),
    this.lineDim = const Color(0xFF14393C),
    this.lineWidth = 1.0,
    this.fg = const Color.fromRGBO(184, 212, 210, 0.76),
    this.fgStrong = const Color.fromRGBO(127, 227, 219, 0.94),
    this.fgDim = const Color.fromRGBO(110, 143, 142, 0.66),
    this.fgMute = const Color.fromRGBO(74, 102, 104, 0.78),
    this.accent = const Color(0xFF58C8C0),
    this.accent2 = const Color(0xFFC8C000),
    this.accentBlue = const Color(0xFF085890),
    this.accentGlow = const Color.fromRGBO(88, 200, 192, 0.45),
    this.stNormal = const Color(0xFF40B040),
    this.stWarn = const Color(0xFFC8C000),
    this.stFault = const Color(0xFFF88070),
    this.stLatched = const Color(0xFFFF9E96),
    this.stStale = const Color(0xFF4A6668),
    this.stPending = const Color(0xFFB8B848),
    this.series1 = const Color(0xFF58C8C0),
    this.series2 = const Color(0xFFC8C000),
    this.series3 = const Color(0xFFFF87D7),
    this.series4 = const Color(0xFFAF87FF),
    this.series5 = const Color(0xFF87FFAF),
    this.series6 = const Color(0xFFF88070),
    this.glow1 = const Color.fromRGBO(127, 227, 219, 0.85),
    this.glow2 = const Color.fromRGBO(88, 200, 192, 0.48),
    this.glow3 = const Color.fromRGBO(88, 200, 192, 0.24),
    this.glow4 = const Color.fromRGBO(88, 200, 192, 0.11),
    this.stroke = const Color.fromRGBO(127, 227, 219, 0.46),
  });

  /// Deepest blue-black, viewport outsides.
  final Color bgVoid;

  /// Main background, measured first place (55.2%) — deep blue-black.
  final Color bgBase;

  /// Panel base, converging to the measured second place cyan-green.
  final Color bgPanel;

  /// Raised surface / table header.
  final Color bgRaise;

  /// Hover surface.
  final Color bgHover;

  /// Active surface.
  final Color bgActive;

  /// Bright 1px hairline (measured primary accent).
  final Color lineHi;

  /// Regular 1px border.
  final Color line;

  /// Dim 1px hairline (second / third line).
  final Color lineDim;

  /// Hairlines are always 1px; depth comes from stacking, never scaling.
  final double lineWidth;

  /// Primary text.
  final Color fg;

  /// Numeric values — the brightest focus.
  final Color fgStrong;

  /// Secondary text.
  final Color fgDim;

  /// Labels / units — almost unlit.
  final Color fgMute;

  /// Cyan primary accent (14,604 px).
  final Color accent;

  /// Yellow secondary accent (4,084 px) — NOT magenta.
  final Color accent2;

  /// Deep blue block accent.
  final Color accentBlue;

  /// Accent glow.
  final Color accentGlow;

  /// Status: normal / link ready.
  final Color stNormal;

  /// Status: approaching threshold.
  final Color stWarn;

  /// Status: tripped.
  final Color stFault;

  /// Status: latched, awaiting clear.
  final Color stLatched;

  /// Status: value is stale.
  final Color stStale;

  /// Status: pending persistence.
  final Color stPending;

  /// Data series 1 (measured cyan).
  final Color series1;

  /// Data series 2 (measured yellow).
  final Color series2;

  /// Data series 3 (vaporwave 213 pink).
  final Color series3;

  /// Data series 4 (vaporwave 141 violet).
  final Color series4;

  /// Data series 5 (vaporwave 121 mint).
  final Color series5;

  /// Data series 6 (measured red).
  final Color series6;

  /// Glow layer 1 — the tight core hugging the glyph.
  final Color glow1;

  /// Glow layer 2 — inner halo.
  final Color glow2;

  /// Glow layer 3 — outer halo.
  final Color glow3;

  /// Glow layer 4 — far halo.
  final Color glow4;

  /// 0.4px outline stroke.
  final Color stroke;

  /// Reads the palette from the nearest theme, falling back to the defaults.
  static GuiPalette of(BuildContext context) =>
      Theme.of(context).extension<GuiPalette>() ?? const GuiPalette();

  @override
  GuiPalette copyWith() => this;

  @override
  GuiPalette lerp(covariant GuiPalette? other, double t) => this;
}
