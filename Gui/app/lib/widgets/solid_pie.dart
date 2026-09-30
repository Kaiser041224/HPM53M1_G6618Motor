import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

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

/// Solid-wedge pie chart (design-system §3.5: solid wedges, no stroke, no
/// separating seam, no shadow).
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
    return CustomPaint(
      size: Size.square(size),
      painter: SolidPiePainter(fractions: fractions, colors: colors),
    );
  }
}

/// Paints [SolidPie.fractions] as solid wedges with no stroke or seam.
class SolidPiePainter extends CustomPainter {
  const SolidPiePainter({required this.fractions, required this.colors});

  /// One share per wedge, in draw order.
  final List<double> fractions;

  /// Fill colour per wedge; wraps around if shorter than [fractions].
  final List<Color> colors;

  /// Angular overlap between adjacent wedges. Antialiased `drawArc` edges can
  /// leave a hairline seam where two wedges meet; a sliver of overlap lets the
  /// next wedge paint over it so the disc reads as one solid fill.
  static const double seamOverlap = 0.002;

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
      canvas.drawArc(
        rect,
        angle,
        sweep + seamOverlap,
        true,
        paints[i % paints.length],
      );
      angle += sweep;
    }
  }

  @override
  bool shouldRepaint(covariant SolidPiePainter oldDelegate) =>
      !listEquals(oldDelegate.fractions, fractions) ||
      !listEquals(oldDelegate.colors, colors);
}
