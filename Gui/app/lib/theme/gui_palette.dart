import 'package:flutter/material.dart';

/// TNO 硅晶之梦 palette (v4, muted screenshot-derived).
///
/// Base surface values are copied verbatim from
/// `Gui/ref/design-system/tokens.css` (pixel-measured). The v4 restyle mutes
/// the v3 colors back toward `Gui/ref/screenshots/`: soft cyan hairlines,
/// grey-white text, olive-amber / soft-green / soft-red values, with saturated
/// pixels reserved for thin borders and small icons. Nothing here may be
/// invented: page code must read semantic tokens through [GuiPalette.of]
/// instead of hard-coding.
@immutable
class GuiPalette extends ThemeExtension<GuiPalette> {
  const GuiPalette({
    this.bgVoid = const Color(0xFF04080C),
    this.bgBase = const Color(0xFF081018),
    this.bgPanel = const Color(0xFF0A161A),
    this.bgRaise = const Color(0xFF102024),
    this.bgHover = const Color(0xFF162C30),
    this.bgActive = const Color(0xFF1C383C),
    this.lineHi = const Color(0xFF3F9E98),
    this.line = const Color(0xFF1E5458),
    this.lineDim = const Color(0xFF123234),
    this.lineWidth = 1.0,
    this.fg = const Color(0xFFA6BAB8),
    this.fgStrong = const Color(0xFFCFDCDA),
    this.fgDim = const Color(0xFF6E908E),
    this.fgMute = const Color(0xFF507270),
    this.valueAmber = const Color(0xFFBFA84C),
    this.valueGreen = const Color(0xFF589E5E),
    this.valueRed = const Color(0xFFB86858),
    this.valuePink = const Color(0xFFC07898),
    this.valueViolet = const Color(0xFF8878B8),
    this.gearGreen = const Color(0xFF589E5E),
    this.gearGreenRim = const Color(0xFF3C6E44),
    this.gearViolet = const Color(0xFF8878B8),
    this.gearVioletRim = const Color(0xFF5E5288),
    this.white = const Color(0xFFCFDCDA),
    this.accent = const Color(0xFF3F9E98),
    this.accent2 = const Color(0xFFBFA84C),
    this.accentBlue = const Color(0xFF085890),
    this.accentGlow = const Color.fromRGBO(63, 158, 152, 0.45),
    this.stNormal = const Color(0xFF4E9454),
    this.stWarn = const Color(0xFFB0A038),
    this.stFault = const Color(0xFFC06858),
    this.stLatched = const Color(0xFFD08880),
    this.stStale = const Color(0xFF445C5E),
    this.stPending = const Color(0xFFA0A048),
    this.btnFill = const Color(0xFF222C2E),
    this.btnBorder = const Color(0xFF3E6466),
    this.btnText = const Color(0xFFA6BAB8),
    this.btnPrimaryBorder = const Color(0xFF3F9E98),
    this.btnPrimaryText = const Color(0xFF6FB8B2),
    this.btnDangerBorder = const Color(0xFFC06858),
    this.btnDangerText = const Color(0xFFD08880),
    this.series1 = const Color(0xFF3F9E98),
    this.series2 = const Color(0xFFBFA84C),
    this.series3 = const Color(0xFFC07898),
    this.series4 = const Color(0xFF8878B8),
    this.series5 = const Color(0xFF589E90),
    this.series6 = const Color(0xFFB86858),
    this.glowSoft = const Color.fromRGBO(63, 158, 152, 0.12),
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

  /// Soft cyan hairline — the v4 frame / underline color.
  final Color lineHi;

  /// Regular border.
  final Color line;

  /// Dark inner seam beside [lineHi].
  final Color lineDim;

  /// Hairlines are always 1px; depth comes from stacking, never scaling.
  final double lineWidth;

  /// Primary text (solid v4 value).
  final Color fg;

  /// Brightest text — grey-white titles and neutral values.
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

  /// Input gear soft fill (encoder schematic, v4 soft-green).
  final Color gearGreen;

  /// Input gear deep rim — one step darker than [gearGreen].
  final Color gearGreenRim;

  /// Output gear soft fill (encoder schematic, v4 soft-violet).
  final Color gearViolet;

  /// Output gear deep rim — one step darker than [gearViolet].
  final Color gearVioletRim;

  /// Neutral solid light-grey (v4), alias of [fgStrong].
  final Color white;

  /// Cyan primary accent.
  final Color accent;

  /// Olive secondary accent — NOT magenta.
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

  /// v4 single-layer soft glow — titles and large values only.
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
