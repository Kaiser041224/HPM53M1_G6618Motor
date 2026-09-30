import 'package:flutter/material.dart';

import 'gui_palette.dart';

/// Glow tiers (v3).
///
/// The screenshot-derived v3 visual keeps only a little light: a single soft
/// layer for titles / large values. The old multi-layer tiers are gone from
/// the general vocabulary and survive only in [GuiGlowStyles.ascii] for the
/// device ASCII block art.
enum GlowStrength {
  /// Single soft layer: large values, headings.
  strong,

  /// Single soft layer: secondary values.
  soft,

  /// A single very faint halo: labels, units.
  weak,
}

/// Glow helpers, v3.
///
/// Glow is carried by `TextStyle.shadows`. v3 avoids stacking layers (a single
/// layer no longer smears because it is deliberately faint):
///
/// - [strong] / [soft] → one [GuiPalette.glowSoft] layer;
/// - [weak] → one faint halo;
/// - [none] → no shadow at all;
/// - [status] → one same-hue layer, statuses must never cross-colorize;
/// - [ascii] → the legacy three-layer same-hue glow, kept for the device art.
class GuiGlowStyles {
  GuiGlowStyles._();

  static const GuiPalette _palette = GuiPalette();

  /// Blur of the v3 soft layer (`0 0 8px`).
  static const double softBlur = 8;

  /// Strong glow: a single soft layer (v3 — the old stroke is gone).
  static TextStyle strong(TextStyle base) =>
      base.copyWith(shadows: <Shadow>[
        Shadow(color: _palette.glowSoft, blurRadius: softBlur),
      ]);

  /// Soft glow: a single soft layer.
  static TextStyle soft(TextStyle base) => softWith(_palette, base);

  /// Soft glow using an explicit [palette].
  ///
  /// Callers that resolved a context palette pass it here so the glow layer
  /// is sourced from the same palette as the rest of the resolved style.
  static TextStyle softWith(GuiPalette palette, TextStyle base) =>
      base.copyWith(shadows: <Shadow>[
        Shadow(color: palette.glowSoft, blurRadius: softBlur),
      ]);

  /// Weak glow: one faint halo, labels recede.
  static TextStyle weak(TextStyle base) => base.copyWith(
    shadows: <Shadow>[
      Shadow(color: _palette.glowSoft.withValues(alpha: 0.15), blurRadius: 4),
    ],
  );

  /// No glow at all.
  static TextStyle none(TextStyle base) => base.copyWith(shadows: const <Shadow>[]);

  /// Same-hue glow for a status color. Statuses must never cross-colorize:
  /// a red fault glows red, a yellow warning glows yellow.
  static TextStyle status(Color color, TextStyle base) => base.copyWith(
    color: color,
    shadows: <Shadow>[
      Shadow(color: color.withValues(alpha: 0.30), blurRadius: softBlur),
    ],
  );

  /// Device ASCII glow: the legacy three-layer same-hue glow, preserved as the
  /// only place the strong multi-layer motif survives (v3).
  static TextStyle ascii(Color color, TextStyle base) => base.copyWith(
    color: color,
    shadows: <Shadow>[
      Shadow(color: color.withValues(alpha: 0.2), blurRadius: 18),
      Shadow(color: color.withValues(alpha: 0.42), blurRadius: 6),
      Shadow(color: color.withValues(alpha: 0.8), blurRadius: 1),
    ],
  );
}
