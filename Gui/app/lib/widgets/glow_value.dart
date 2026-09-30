import 'package:flutter/material.dart';

import '../theme/gui_glow.dart';
import '../theme/gui_type.dart';

/// Outlined glowing text, the core typographic motif of the design system.
///
/// [strength] selects the glow tier: [GlowStrength.strong] adds the 0.4px
/// stroke on top of the three glow layers (see [GuiGlowStyles]), [GlowStrength
/// .soft] keeps only the inner + outer halo, and [GlowStrength.weak] leaves a
/// single faint halo for labels.
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

/// A right-aligned, tabular number with unit.
///
/// Precision is dimension driven (current 3, bus 1, frequency 2 — see
/// design-system §5), never a global constant. A null [value] renders an
/// em-dash so a stale reading can never masquerade as a live one
/// (design-system §6, rule 2).
class GlowValue extends StatelessWidget {
  const GlowValue({
    super.key,
    required this.value,
    this.unit,
    this.precision = 3,
  });

  /// The number, or null when the reading is stale / unknown.
  final num? value;

  /// Optional unit, rendered uppercase in the label tier.
  final String? unit;

  /// Digits after the decimal point, chosen by dimension.
  final int precision;

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    final text = value == null ? '—' : value!.toStringAsFixed(precision);
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: [
        GlowText(text, style: scale.data, textAlign: TextAlign.right),
        if (unit != null) ...[
          const SizedBox(width: 4),
          Text(unit!.toUpperCase(), style: GuiGlowStyles.weak(scale.label)),
        ],
      ],
    );
  }
}
