import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Freshness of an otherwise "live" reading (design-system §6, rule 2).
enum Freshness {
  /// Current: the live widget is trustworthy.
  live,

  /// Poll failed: the old value must not pass itself off as current.
  stale,

  /// Never read / unknown.
  unknown,
}

/// Paints a dashed 1px rectangle of [color] around [child].
///
/// Flutter has no dashed [Border], so the dashes are stroked straight onto the
/// canvas. Used to mark readings that are not fresh.
class DashedFrame extends StatelessWidget {
  const DashedFrame({super.key, required this.color, required this.child});

  /// Dash color.
  final Color color;

  /// Framed content.
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      painter: _DashedRectPainter(color),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
        child: child,
      ),
    );
  }
}

class _DashedRectPainter extends CustomPainter {
  const _DashedRectPainter(this.color);

  final Color color;

  static const double _dash = 4;
  static const double _gap = 3;

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1;

    void dashedLine(Offset a, Offset b) {
      final total = (b - a).distance;
      if (total <= 0) return;
      final direction = (b - a) / total;
      for (var start = 0.0; start < total; start += _dash + _gap) {
        final end = math.min(start + _dash, total);
        canvas.drawLine(a + direction * start, a + direction * end, paint);
      }
    }

    final w = size.width;
    final h = size.height;
    dashedLine(Offset.zero, Offset(w, 0));
    dashedLine(Offset(w, 0), Offset(w, h));
    dashedLine(Offset(w, h), Offset(0, h));
    dashedLine(Offset(0, h), Offset.zero);
  }

  @override
  bool shouldRepaint(covariant _DashedRectPainter oldDelegate) =>
      oldDelegate.color != color;
}

/// Guards a live reading behind its [freshness].
///
/// When live, [live] is shown untouched — including a real `0`, which must
/// never be confused with the em-dash fallback. When stale or unknown the
/// value is replaced by [fallback] inside a dashed frame and carries no glow,
/// so a failed poll can never masquerade as a fresh reading
/// (design-system §6, rule 2).
class StaleGate extends StatelessWidget {
  const StaleGate({
    super.key,
    required this.freshness,
    required this.live,
    this.fallback = '—',
  });

  /// Trust level of the reading.
  final Freshness freshness;

  /// The widget rendered when fresh.
  final Widget live;

  /// Placeholder shown when not fresh.
  final String fallback;

  @override
  Widget build(BuildContext context) {
    if (freshness == Freshness.live) {
      return live;
    }
    final palette = GuiPalette.of(context);
    final color = freshness == Freshness.stale
        ? palette.stStale
        : palette.fgMute;
    return DashedFrame(
      color: color,
      child: Text(
        fallback,
        style: GuiTypeScale.of(
          context,
        ).data.copyWith(color: color, shadows: const <Shadow>[]),
      ),
    );
  }
}
