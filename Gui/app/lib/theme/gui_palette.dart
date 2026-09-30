import 'package:flutter/material.dart';

/// TNO 硅晶之梦 palette (v3, screenshot-derived).
///
/// Base surface values are copied verbatim from
/// `Gui/ref/design-system/tokens.css` (pixel-measured); the v3 restyle
/// reinterprets their usage toward flat, bright hairline frames and solid
/// semantic text. Nothing here may be invented: page code must read
/// semantic tokens through [GuiPalette.of] instead of hard-coding.
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
    this.fg = const Color(0xFFC8DEDC),
    this.fgStrong = const Color(0xFFEAF6F4),
    this.fgDim = const Color(0xFF7FA8A6),
    this.fgMute = const Color(0xFF5E8886),
    this.valueAmber = const Color(0xFFE8C84A),
    this.valueGreen = const Color(0xFF5ED85E),
    this.valueRed = const Color(0xFFF06858),
    this.valuePink = const Color(0xFFE888B8),
    this.valueViolet = const Color(0xFFA888E8),
    this.white = const Color(0xFFEAF6F4),
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
    this.btnFill = const Color(0xFF243234),
    this.btnBorder = const Color(0xFF4E7A7C),
    this.btnText = const Color(0xFFC8DEDC),
    this.btnPrimaryBorder = const Color(0xFF58C8C0),
    this.btnPrimaryText = const Color(0xFF7AD8D0),
    this.btnDangerBorder = const Color(0xFFC85848),
    this.btnDangerText = const Color(0xFFF08878),
    this.series1 = const Color(0xFF58C8C0),
    this.series2 = const Color(0xFFC8C000),
    this.series3 = const Color(0xFFFF87D7),
    this.series4 = const Color(0xFFAF87FF),
    this.series5 = const Color(0xFF87FFAF),
    this.series6 = const Color(0xFFF88070),
    this.glowSoft = const Color.fromRGBO(88, 200, 192, 0.22),
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

  /// Bright cyan hairline — the v3 frame / underline color.
  final Color lineHi;

  /// Regular border.
  final Color line;

  /// Dark inner seam beside [lineHi].
  final Color lineDim;

  /// Hairlines are always 1px; depth comes from stacking, never scaling.
  final double lineWidth;

  /// Primary text (solid v3 value).
  final Color fg;

  /// Brightest text — white titles and neutral values.
  final Color fgStrong;

  /// Secondary text.
  final Color fgDim;

  /// Labels / units.
  final Color fgMute;

  /// Flat semantic value: key value / cost / duration.
  final Color valueAmber;

  /// Flat semantic value: healthy / positive.
  final Color valueGreen;

  /// Flat semantic value: fault / negative.
  final Color valueRed;

  /// Flat semantic value: accent series.
  final Color valuePink;

  /// Flat semantic value: accent series.
  final Color valueViolet;

  /// Neutral solid white (v3), alias of [fgStrong].
  final Color white;

  /// Cyan primary accent.
  final Color accent;

  /// Yellow secondary accent — NOT magenta.
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

  /// Button fill (all three tiers share it).
  final Color btnFill;

  /// Normal button border.
  final Color btnBorder;

  /// Normal button text.
  final Color btnText;

  /// Primary (main action) button border.
  final Color btnPrimaryBorder;

  /// Primary (main action) button text.
  final Color btnPrimaryText;

  /// Danger button border.
  final Color btnDangerBorder;

  /// Danger button text.
  final Color btnDangerText;

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

  /// v3 single-layer soft glow — titles and large values only.
  final Color glowSoft;

  /// Glow layer 1 — legacy core (ASCII device glow only).
  final Color glow1;

  /// Glow layer 2 — legacy inner halo (ASCII device glow only).
  final Color glow2;

  /// Glow layer 3 — legacy outer halo.
  final Color glow3;

  /// Glow layer 4 — legacy far halo.
  final Color glow4;

  /// 0.4px outline stroke (legacy).
  final Color stroke;

  /// Reads the palette from the nearest theme, falling back to the defaults.
  static GuiPalette of(BuildContext context) =>
      Theme.of(context).extension<GuiPalette>() ?? const GuiPalette();

  @override
  GuiPalette copyWith() => this;

  @override
  GuiPalette lerp(covariant GuiPalette? other, double t) => this;
}
