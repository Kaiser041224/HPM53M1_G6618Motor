import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';

/// LED / oscilloscope tick vocabulary: a row of fixed-height solid segments
/// separated by fixed gaps, never a smooth progress bar (design-system §3.4).
///
/// The lit count is `(value01 * segments).round()` clamped to `[0, segments]`.
/// Lit segments use [GuiPalette.accent], or [GuiPalette.accent2] when [warn] is
/// set, plus a glow of the very same hue. Whether a value deserves `warn` is
/// the caller's decision — this widget only paints what it is told.
class SegmentBar extends StatelessWidget {
  const SegmentBar({
    super.key,
    required this.value01,
    this.segments = 24,
    this.warn = false,
  });

  /// Normalized value, nominally `[0, 1]`; clamped when rendered.
  final double value01;

  /// Total number of segments.
  final int segments;

  /// When true the lit segments take the secondary accent (yellow).
  final bool warn;

  /// Height of one segment (`--seg-height`, measured 9px).
  static const double segmentHeight = 9;

  /// Gap between segments (`--seg-gap`, measured 2px).
  static const double segmentGap = 2;

  /// Alpha of the lit-segment halo (v4: a weak same-hue glow).
  static const double glowAlpha = 0.12;

  /// Number of lit segments for [value01]: rounds, then clamps.
  static int litCount(double value01, int segments) =>
      (value01 * segments).round().clamp(0, segments);

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final on = warn ? palette.accent2 : palette.accent;
    final lit = litCount(value01, segments);
    return SizedBox(
      height: segmentHeight,
      child: Row(
        children: <Widget>[
          for (var i = 0; i < segments; i++) ...<Widget>[
            if (i > 0) const SizedBox(width: segmentGap),
            Expanded(
              child: Container(
                decoration: BoxDecoration(
                  color: i < lit ? on : palette.bgRaise,
                  boxShadow: i < lit
                      ? <BoxShadow>[
                          BoxShadow(
                            color: on.withValues(alpha: glowAlpha),
                            blurRadius: 8,
                          ),
                        ]
                      : null,
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }
}
