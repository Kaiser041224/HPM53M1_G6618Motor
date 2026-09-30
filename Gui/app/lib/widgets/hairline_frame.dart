import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';

/// Hairline frame variants (design-system §3.2).
enum HairlineVariant {
  /// Bright line, an inner dark seam, an outer dim line and a faint glow.
  main,

  /// A single dim line plus a very faint inner ring, for nested blocks.
  sub,
}

/// Multi-hairline rectangular frame; always square, always 1px lines.
///
/// Thickness comes from stacking lines, never from scaling them
/// (design-system §3.2).
class HairlineFrame extends StatelessWidget {
  const HairlineFrame({
    super.key,
    required this.child,
    this.variant = HairlineVariant.main,
    this.padding = EdgeInsets.zero,
  });

  /// Wrapped content.
  final Widget child;

  /// Which line motif to draw.
  final HairlineVariant variant;

  /// Inner padding applied inside the frame.
  final EdgeInsetsGeometry padding;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    if (variant == HairlineVariant.sub) {
      return Container(
        decoration: BoxDecoration(
          border: Border.all(color: palette.lineDim, width: palette.lineWidth),
        ),
        child: Container(
          decoration: BoxDecoration(
            border: Border.all(
              color: palette.accent.withValues(alpha: 0.06),
              width: palette.lineWidth,
            ),
          ),
          padding: padding,
          child: child,
        ),
      );
    }
    return Container(
      decoration: BoxDecoration(
        border: Border.all(color: palette.lineHi, width: palette.lineWidth),
        boxShadow: <BoxShadow>[
          BoxShadow(color: palette.lineDim, spreadRadius: 1),
          BoxShadow(color: palette.accent.withValues(alpha: 0.10), blurRadius: 14),
        ],
      ),
      child: Container(
        decoration: BoxDecoration(
          border: Border.all(color: palette.bgPanel, width: palette.lineWidth),
        ),
        padding: padding,
        child: child,
      ),
    );
  }
}

/// The primary section divider: one bright line plus two dim lines below it
/// (design-system §3.2 "一亮两暗"). Dividers are scarce — use sparingly.
class HairlineDivider extends StatelessWidget {
  const HairlineDivider({super.key});

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Container(height: palette.lineWidth, color: palette.lineHi),
        SizedBox(height: palette.lineWidth),
        Container(height: palette.lineWidth, color: palette.lineDim),
        const SizedBox(height: 4),
        Container(height: palette.lineWidth, color: palette.lineDim),
      ],
    );
  }
}
