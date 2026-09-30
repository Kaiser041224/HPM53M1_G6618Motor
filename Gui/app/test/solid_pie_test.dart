import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_palette.dart';
import 'package:g6618_gui/widgets/solid_pie.dart';

void main() {
  group('phaseCurrentFractions', () {
    test('uses absolute magnitudes: 1:2:1 -> 25/50/25 %', () {
      final f = phaseCurrentFractions(1, 2, 1);
      expect(f, isNotNull);
      expect(f![0], closeTo(0.25, 1e-9));
      expect(f[1], closeTo(0.50, 1e-9));
      expect(f[2], closeTo(0.25, 1e-9));
    });

    test('sign does not matter (abs of negative phase currents)', () {
      final f = phaseCurrentFractions(-1, 2, -1);
      expect(f![0], closeTo(0.25, 1e-9));
      expect(f[1], closeTo(0.50, 1e-9));
      expect(f[2], closeTo(0.25, 1e-9));
    });

    test('all-zero is undefined -> null (honest —, not equal thirds)', () {
      expect(phaseCurrentFractions(0, 0, 0), isNull);
    });

    test('a missing reading is undefined -> null', () {
      expect(phaseCurrentFractions(1, null, 1), isNull);
      expect(phaseCurrentFractions(null, null, null), isNull);
    });
  });

  group('SolidPiePainter', () {
    const palette = GuiPalette();

    test('wedges are solid fills with no stroke (design-system §3.5)', () {
      final painter = SolidPiePainter(
        fractions: const <double>[0.25, 0.5, 0.25],
        colors: <Color>[palette.series1, palette.series2, palette.series3],
      );
      final paints = painter.buildPaints();
      expect(paints, hasLength(3));
      for (final paint in paints) {
        expect(paint.style, PaintingStyle.fill);
        expect(paint.strokeWidth, 0);
      }
    });

    test('repaints when the fractions change', () {
      final a = SolidPiePainter(
        fractions: const <double>[1, 0, 0],
        colors: const <Color>[Colors.red, Colors.green, Colors.blue],
      );
      final b = SolidPiePainter(
        fractions: const <double>[0, 1, 0],
        colors: const <Color>[Colors.red, Colors.green, Colors.blue],
      );
      expect(a.shouldRepaint(b), isTrue);
      expect(a.shouldRepaint(a), isFalse);
    });
  });
}
