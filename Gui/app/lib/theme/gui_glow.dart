import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import 'gui_palette.dart';

/// Glow tiers, see design-system §2.4 "分层给光，不是整屏发光".
enum GlowStrength {
  /// Four layers + stroke: large values, device art, headings.
  strong,

  /// Inner + outer halo: secondary values, legend, bars.
  soft,

  /// A single faint 4px halo: labels, units, table headers.
  weak,
}

/// Multi-layer outline glow helpers.
///
/// Implementation notes (design-system §2.4): glow is carried by
/// `TextStyle.shadows`, split into core / inner / outer / far layers. A single
/// layer smears, so the layers are kept distinct. The 0.4px outline is applied
/// as a `foreground` stroke Paint for the [strong] tier; because Flutter's
/// `TextStyle` forbids `color` and `foreground` together, the glyph fill is
/// supplied by a zero-blur, zero-offset [Shadow] placed on top of the glow
/// layers. This yields fill + true stroke + glow on a single [Text] node.
class GuiGlowStyles {
  GuiGlowStyles._();

  static const GuiPalette _palette = GuiPalette();

  /// Builds a stroked + filled style. [fill] defaults to the base text color.
  static TextStyle _outlined(TextStyle base, Color fill) {
    final stroke = ui.Paint()
      ..style = ui.PaintingStyle.stroke
      ..strokeWidth = 0.4
      ..color = _palette.stroke;
    return TextStyle(
      inherit: base.inherit,
      fontFamily: base.fontFamily,
      fontFamilyFallback: base.fontFamilyFallback,
      fontSize: base.fontSize,
      fontWeight: base.fontWeight,
      fontStyle: base.fontStyle,
      letterSpacing: base.letterSpacing,
      wordSpacing: base.wordSpacing,
      height: base.height,
      fontFeatures: base.fontFeatures,
      decoration: base.decoration,
      foreground: stroke,
      shadows: <Shadow>[
        Shadow(color: _palette.glow3, blurRadius: 18),
        Shadow(color: _palette.glow2, blurRadius: 6),
        Shadow(color: _palette.glow1, blurRadius: 1),
        Shadow(color: fill, blurRadius: 0, offset: Offset.zero),
      ],
    );
  }

  /// Strong glow: core + inner + outer halo, plus the 0.4px stroke.
  static TextStyle strong(TextStyle base) =>
      _outlined(base, base.color ?? _palette.fgStrong);

  /// Soft glow: inner + outer halo only.
  static TextStyle soft(TextStyle base) => base.copyWith(
    shadows: <Shadow>[
      Shadow(color: _palette.glow3, blurRadius: 18),
      Shadow(color: _palette.glow2, blurRadius: 6),
    ],
  );

  /// Wide glow: all four layers, for large surfaces / big text.
  static TextStyle wide(TextStyle base) => base.copyWith(
    shadows: <Shadow>[
      Shadow(color: _palette.glow4, blurRadius: 34),
      Shadow(color: _palette.glow3, blurRadius: 18),
      Shadow(color: _palette.glow2, blurRadius: 6),
      Shadow(color: _palette.glow1, blurRadius: 1),
    ],
  );

  /// Weak glow: one faint 4px inner halo, labels recede.
  static TextStyle weak(TextStyle base) => base.copyWith(
    shadows: <Shadow>[
      Shadow(color: _palette.glow2.withValues(alpha: 0.15), blurRadius: 4),
    ],
  );

  /// No glow at all.
  static TextStyle none(TextStyle base) => base.copyWith(shadows: const <Shadow>[]);

  /// Same-hue glow for a status color. Statuses must never cross-colorize:
  /// a red fault glows red, a yellow warning glows yellow (design-system §2.4).
  static TextStyle status(Color color, TextStyle base) => base.copyWith(
    color: color,
    shadows: <Shadow>[
      Shadow(color: color.withValues(alpha: 0.2), blurRadius: 18),
      Shadow(color: color.withValues(alpha: 0.42), blurRadius: 6),
      Shadow(color: color.withValues(alpha: 0.8), blurRadius: 1),
    ],
  );
}
