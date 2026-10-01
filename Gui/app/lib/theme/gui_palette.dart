import 'package:flutter/material.dart';

/// TNO 硅晶之梦 palette (v5, emissive six-color spectrum).
///
/// Base surface values are copied verbatim from
/// `Gui/ref/design-system/tokens.css` (pixel-measured). The v5 restyle replaces
/// the v4 muted screenshot-derived values with an **emissive** vocabulary:
/// a six-color spectrum (cyan / violet / magenta / green / orange / red) tuned
/// for a "machine panel turned into a glowing screen", bright cyan chrome,
/// white-cyan values and purple group lines. Nothing here may be invented:
/// page code must read semantic tokens through [GuiPalette.of] instead of
/// hard-coding.
@immutable
class GuiPalette extends ThemeExtension<GuiPalette> {
  const GuiPalette({
    this.bgVoid = const Color(0xFF04080C),
    this.bgBase = const Color(0xFF081018),
    this.bgPanel = const Color(0xFF0A161A),
    this.bgRaise = const Color(0xFF102024),
    this.bgHover = const Color(0xFF162C30),
    this.bgActive = const Color(0xFF1C383C),
    this.lineHi = const Color(0xFF2ED8E8),
    this.line = const Color(0xFF173E44),
    this.lineDim = const Color(0xFF0F2A2E),
    this.groupLine = const Color(0x808A4DFF),
    this.lineWidth = 1.0,
    this.fg = const Color(0xFFB2C9C2),
    this.fgStrong = const Color(0xFFB8F4F8),
    this.fgDim = const Color(0xFF4E8E92),
    // v5 最暗文字档：退到背景里的标签 / 单位 / 注记（刻意不达正文对比下限）。
    this.fgMute = const Color(0xFF3E6E72),
    // v4 暗金：保留字段以兼容既有调用，v5 UI 不再用于条形读数。
    this.valueAmber = const Color(0xFFBFA84C),
    this.valueCyan = const Color(0xFF2ED8E8),
    this.valueGreen = const Color(0xFF3DDB6E),
    this.valueOrange = const Color(0xFFF08030),
    this.valueRed = const Color(0xFFFF4D4D),
    this.valueMagenta = const Color(0xFFE040C8),
    this.valuePurple = const Color(0xFF8A4DFF),
    this.gearGreen = const Color(0xFF589E5E),
    this.gearGreenRim = const Color(0xFF3C6E44),
    this.gearViolet = const Color(0xFF8878B8),
    this.gearVioletRim = const Color(0xFF5E5288),
    this.white = const Color(0xFFB8F4F8),
    this.accent = const Color(0xFF2ED8E8),
    this.accent2 = const Color(0xFFBFA84C),
    this.accentBlue = const Color(0xFF085890),
    this.accentGlow = const Color.fromRGBO(63, 158, 152, 0.45),
    this.stNormal = const Color(0xFF3DDB6E),
    this.stWarn = const Color(0xFFF08030),
    this.stFault = const Color(0xFFFF4D4D),
    this.stLatched = const Color(0xFFFF8080),
    // 失效态是**安全提示**：必须看得出"这里没数据"。取值保证大字下限 3.0，
    // 且明显暗于正常态，保住"退后"的语义。
    this.stStale = const Color(0xFF4E6E72),
    this.stPending = const Color(0xFFC8B848),
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
    this.glowSoft = const Color.fromRGBO(46, 216, 232, 0.12),
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

  /// Bright cyan hairline — the v5 panel frame / underline color.
  final Color lineHi;

  /// Regular row divider — low-brightness cyan.
  final Color line;

  /// Dark inner seam beside [lineHi].
  final Color lineDim;

  /// Title / group underline — low-brightness violet (role-fixed, no rotation).
  final Color groupLine;

  /// Hairlines are always 1px; depth comes from stacking, never scaling.
  final double lineWidth;

  /// Primary body text (v5).
  final Color fg;

  /// Brightest text — white-cyan titles and neutral values.
  final Color fgStrong;

  /// Secondary text.
  final Color fgDim;

  /// Labels / units / annotations — the dimmest text tier.
  final Color fgMute;

  /// Deprecated (v4 dark gold). Kept for compatibility; v5 bars no longer use it.
  final Color valueAmber;

  /// Dimension accent: voltage `V`, angle `°` — cyan.
  final Color valueCyan;

  /// Dimension accent: current `A`, healthy / low position — green.
  final Color valueGreen;

  /// Dimension accent: temperature `°C`, mid position / warning — orange.
  final Color valueOrange;

  /// Dimension accent: high position / fault — red.
  final Color valueRed;

  /// Dimension accent: power `W`, `%`, emphasis / peak line — magenta.
  final Color valueMagenta;

  /// Dimension accent: frequency `Hz`, EST / inverse values — violet.
  final Color valuePurple;

  /// Input gear soft fill (encoder schematic, v4 soft-green).
  final Color gearGreen;

  /// Input gear deep rim — one step darker than [gearGreen].
  final Color gearGreenRim;

  /// Output gear soft fill (encoder schematic, v4 soft-violet).
  final Color gearViolet;

  /// Output gear deep rim — one step darker than [gearViolet].
  final Color gearVioletRim;

  /// Neutral value white-cyan (v5), alias of [fgStrong] — neutral values / units.
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
