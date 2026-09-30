import 'package:flutter/material.dart';

import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Flat semantic value tones (v3).
enum Tone {
  /// Neutral / default — renders in [GuiPalette.fgStrong].
  neutral,

  /// Key value / cost / duration.
  amber,

  /// Healthy / positive.
  green,

  /// Fault / negative.
  red,

  /// Accent series.
  pink,

  /// Accent series.
  violet,
}

/// Softly glowing text (v3).
///
/// [strength] selects the glow tier: [GlowStrength.strong] and
/// [GlowStrength.soft] both add the single soft layer, [GlowStrength.weak]
/// leaves one very faint halo for labels.
class GlowText extends StatelessWidget {
  const GlowText(
    this.text, {
    super.key,
    this.strength = GlowStrength.strong,
    this.style,
    this.textAlign,
    this.maxLines,
    this.overflow,
  });

  /// The string to render.
  final String text;

  /// Glow tier.
  final GlowStrength strength;

  /// Base style; defaults to the ambient tabular value scale.
  final TextStyle? style;

  /// Horizontal alignment.
  final TextAlign? textAlign;

  /// Optional line cap.
  final int? maxLines;

  /// How to handle overflow.
  final TextOverflow? overflow;

  @override
  Widget build(BuildContext context) {
    final base = style ?? GuiTypeScale.of(context).data;
    final TextStyle effective = switch (strength) {
      GlowStrength.strong => GuiGlowStyles.strong(base),
      GlowStrength.soft => GuiGlowStyles.soft(base),
      GlowStrength.weak => GuiGlowStyles.weak(base),
    };
    return Text(
      text,
      style: effective,
      textAlign: textAlign,
      maxLines: maxLines,
      overflow: overflow,
    );
  }
}

/// A right-aligned, tabular number with unit, painted flat (v3).
///
/// [tone] picks the semantic value color; [glow] optionally stacks the single
/// soft layer (default off, so values read as flat as the screenshots).
/// Precision is dimension driven (current 3, bus 1, frequency 2), never a
/// global constant. A null [value] renders an em-dash so a stale reading can
/// never masquerade as a live one.
class GlowValue extends StatelessWidget {
  const GlowValue({
    super.key,
    required this.value,
    this.unit,
    this.precision = 3,
    this.tone = Tone.neutral,
    this.glow = false,
  });

  /// The number, or null when the reading is stale / unknown.
  final num? value;

  /// Optional unit, rendered uppercase in the label tier.
  final String? unit;

  /// Digits after the decimal point, chosen by dimension.
  final int precision;

  /// Semantic value color.
  final Tone tone;

  /// When true, stack the single soft glow layer on top of the flat color.
  final bool glow;

  /// The palette color for [tone].
  static Color colorFor(GuiPalette palette, Tone tone) => switch (tone) {
    Tone.neutral => palette.fgStrong,
    Tone.amber => palette.valueAmber,
    Tone.green => palette.valueGreen,
    Tone.red => palette.valueRed,
    Tone.pink => palette.valuePink,
    Tone.violet => palette.valueViolet,
  };

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final text = value == null ? '—' : value!.toStringAsFixed(precision);
    final color = value == null ? palette.fgMute : colorFor(palette, tone);
    final style = scale.data.copyWith(
      color: color,
      shadows: (glow && value != null)
          ? <Shadow>[
              Shadow(color: palette.glowSoft, blurRadius: GuiGlowStyles.softBlur),
            ]
          : const <Shadow>[],
    );
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: [
        Text(text, style: style, textAlign: TextAlign.right),
        if (unit != null) ...[
          const SizedBox(width: 4),
          Text(unit!.toUpperCase(), style: scale.label),
        ],
      ],
    );
  }
}
