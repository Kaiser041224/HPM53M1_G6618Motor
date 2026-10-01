import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import 'vfd_bar_painter.dart';

/// VFD 竖排发光管条 (design-system §2.6.5).
///
/// The v5 successor of the v4 fixed-segment LED bar: a continuous bank of
/// vertical luminous tubes in a frosted package, reusing [VfdBarPainter].
/// [value01] is the lit fraction and [tint] overrides the tube colour; by
/// default the cyan accent is used, switching to the warning orange when
/// [warn] is set. [segments] is a legacy parameter retained so existing call
/// sites keep compiling — the VFD bar is continuous and ignores it.
class SegmentBar extends StatelessWidget {
  const SegmentBar({
    super.key,
    required this.value01,
    this.segments = 24,
    this.warn = false,
    this.tint,
    this.height = 14,
  });

  /// Normalized value, nominally `[0, 1]`; clamped when rendered.
  final double value01;

  /// Legacy segment count (v4), unused by the continuous VFD rendering.
  final int segments;

  /// When true the tube tint switches to the warning orange.
  final bool warn;

  /// Explicit tube tint; when null it is derived from [warn].
  final Color? tint;

  /// Bar height, px.
  final double height;

  /// Legacy segment height (v4), kept for compatibility.
  static const double segmentHeight = 9;

  /// Legacy gap between segments (v4), kept for compatibility.
  static const double segmentGap = 2;

  /// Legacy lit-segment halo alpha (v4), kept for compatibility.
  static const double glowAlpha = 0.12;

  /// Number of lit segments for [value01]: rounds, then clamps. Retained for
  /// compatibility; the continuous VFD bar does not use it.
  static int litCount(double value01, int segments) =>
      (value01 * segments).round().clamp(0, segments);

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final color = tint ?? (warn ? palette.valueOrange : palette.accent);
    return VfdBar(
      ratio: value01,
      tint: color,
      warnBand: false,
      height: height,
    );
  }
}
