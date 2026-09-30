import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';

/// Hairline frame variants (design-system §3.2, v3).
enum HairlineVariant {
  /// A single bright cyan border with a dark inner seam.
  main,

  /// A single dim border, for nested blocks.
  sub,
}

/// Rectangular hairline frame; always square.
///
/// v3 drops the stacked "一亮两暗" lines and the glow: a bright 1.5px cyan
/// border carries the frame, with a 1px dark seam just inside it for depth.
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

  /// Width of the bright v3 frame line.
  static const double brightWidth = 1.5;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    if (variant == HairlineVariant.sub) {
      return Container(
        decoration: BoxDecoration(
          border: Border.all(color: palette.lineDim, width: palette.lineWidth),
        ),
        padding: padding,
        child: child,
      );
    }
    return Container(
      decoration: BoxDecoration(
        border: Border.all(color: palette.lineHi, width: brightWidth),
      ),
      child: Container(
        decoration: BoxDecoration(
          border: Border.all(color: palette.lineDim, width: palette.lineWidth),
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
