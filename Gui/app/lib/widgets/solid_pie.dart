import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';

/// Absolute-magnitude share of the three phase currents, or null when the
/// share is undefined.
///
/// The share is the honest ratio `|I_U| : |I_V| : |I_W|`. When every phase is
/// zero (or any reading is missing) the ratio is undefined and null is
/// returned, so the UI can show `—` instead of fabricating equal thirds
/// (design-system §6 rule 2).
List<double>? phaseCurrentFractions(num? iu, num? iv, num? iw) {
  if (iu == null || iv == null || iw == null) return null;
  final u = iu.abs().toDouble();
  final v = iv.abs().toDouble();
  final w = iw.abs().toDouble();
  final total = u + v + w;
  if (total <= 0) return null;
  return <double>[u / total, v / total, w / total];
}

/// Solid-wedge pie chart (v3: still no outer glow, now with 1px dark seams
/// between wedges).
///
/// [fractions] must be non-negative and normally sum to 1; [colors] supplies
/// one fill per wedge. The first wedge starts at 12 o'clock.
class SolidPie extends StatelessWidget {
  const SolidPie({
    super.key,
    required this.fractions,
    required this.colors,
    this.size = defaultSize,
  });

  /// Default diameter (`--pie` in the design template).
  static const double defaultSize = 128;

  /// One share per wedge, in draw order.
  final List<double> fractions;

  /// Fill colour per wedge; wraps around if shorter than [fractions].
  final List<Color> colors;

  /// Diameter of the pie.
  final double size;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    return CustomPaint(
      size: Size.square(size),
      painter: SolidPiePainter(
        fractions: fractions,
        colors: colors,
        seamColor: palette.lineDim,
      ),
    );
  }
}

/// Paints [SolidPie.fractions] as solid wedges separated by 1px dark seams.
class SolidPiePainter extends CustomPainter {
  const SolidPiePainter({
    required this.fractions,
    required this.colors,
    required this.seamColor,
  });

  /// One share per wedge, in draw order.
  final List<double> fractions;

  /// Fill colour per wedge; wraps around if shorter than [fractions].
  final List<Color> colors;

  /// Dark color of the 1px radial seam drawn between adjacent wedges (v3).
  final Color seamColor;

  /// Width of the radial seam (v3).
  static const double seamWidth = 1;

  /// The fill-only paints, one per colour. Exposed so the "solid, no stroke"
  /// invariant (design-system §3.5) can be asserted without a golden.
  @visibleForTesting
  List<Paint> buildPaints() => <Paint>[
    for (final color in colors)
      Paint()
        ..color = color
        ..style = PaintingStyle.fill
        ..strokeWidth = 0
        ..isAntiAlias = true,
  ];

  @override
  void paint(Canvas canvas, Size size) {
    if (fractions.isEmpty || colors.isEmpty) return;
    final rect = Offset.zero & size;
    final paints = buildPaints();
    const start = -math.pi / 2;
    var angle = start;
    for (var i = 0; i < fractions.length; i++) {
      final sweep = fractions[i] * 2 * math.pi;
      if (sweep <= 0) continue;
      canvas.drawArc(rect, angle, sweep, true, paints[i % paints.length]);
      angle += sweep;
    }
    if (fractions.length > 1) {
      final seamPaint = Paint()
        ..color = seamColor
        ..style = PaintingStyle.stroke
        ..strokeWidth = seamWidth;
      final center = rect.center;
      final radius = size.shortestSide / 2;
      var boundary = start;
      for (final fraction in fractions) {
        boundary += fraction * 2 * math.pi;
        final end =
            center + Offset(math.cos(boundary), math.sin(boundary)) * radius;
        canvas.drawLine(center, end, seamPaint);
      }
    }
  }

  @override
  bool shouldRepaint(covariant SolidPiePainter oldDelegate) =>
      !listEquals(oldDelegate.fractions, fractions) ||
      !listEquals(oldDelegate.colors, colors) ||
      oldDelegate.seamColor != seamColor;
}
