import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/encoder_schematic.dart';

Future<void> _pump(WidgetTester tester, Widget child) {
  tester.view.physicalSize = const Size(1400, 1000);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(body: SingleChildScrollView(child: child)),
    ),
  );
}

EncoderSchematicPainter _painter(WidgetTester tester) {
  final paint = tester.widget<CustomPaint>(
    find.byWidgetPredicate(
      (w) => w is CustomPaint && w.painter is EncoderSchematicPainter,
    ),
  );
  return paint.painter! as EncoderSchematicPainter;
}

void main() {
  group('dial geometry', () {
    test('0° points up and 90° points right (0°=12点, clockwise positive)', () {
      expect(dialRadians(0), closeTo(-math.pi / 2, 1e-9));
      expect(dialRadians(90), closeTo(0, 1e-9));
    });

    test('pointer tip follows the angle (0° above, 90° right of centre)', () {
      const centre = Offset(100, 100);
      const radius = 10.0;
      final up = pointerTip(centre, radius, 0);
      final right = pointerTip(centre, radius, 90);
      expect(up.dx, closeTo(100, 1e-9));
      expect(up.dy, closeTo(90, 1e-9));
      expect(right.dx, closeTo(110, 1e-9));
      expect(right.dy, closeTo(100, 1e-9));
      expect((up - centre).distance, closeTo(radius, 1e-9));
      expect((right - centre).distance, closeTo(radius, 1e-9));
    });
  });

  group('gearTeethPair', () {
    test('unloaded ratio is a 1:1 pair', () {
      expect(gearTeethPair('—'), (14, 14));
    });

    test('teeth approximate the ratio, clamped to 8..20', () {
      expect(gearTeethPair('3.50'), (14, 20));
      expect(gearTeethPair('0.50'), (14, 8));
      expect(gearTeethPair('1.00'), (14, 14));
    });

    test('every derived tooth count stays inside 8..20', () {
      for (final label in const <String>['0.01', '0.98', '1.0', '12', '99']) {
        final (left, right) = gearTeethPair(label);
        expect(left, inInclusiveRange(8, 20), reason: label);
        expect(right, inInclusiveRange(8, 20), reason: label);
      }
    });
  });

  group('EncoderSchematic widget', () {
    testWidgets('renders two dials: R rotor and O output', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '1.00'),
      );

      expect(find.byKey(const Key('encoder_dial_rotor')), findsOneWidget);
      expect(find.byKey(const Key('encoder_dial_output')), findsOneWidget);
      final painter = _painter(tester);
      expect(painter.rotorDeg, 0);
      expect(painter.outputDeg, 0);
    });

    testWidgets('pointer channels track their own angle', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(
          rotorDeg: 90,
          outputDeg: 15,
          ratioLabel: '1.00',
        ),
      );
      final painter = _painter(tester);
      expect(painter.rotorDeg, 90);
      expect(painter.outputDeg, 15);
    });

    testWidgets('ratio label renders as i = 3.50 : 1', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '3.50'),
      );
      expect(find.text('i = 3.50 : 1'), findsOneWidget);
    });

    testWidgets('null ratio renders i = —', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '—'),
      );
      expect(find.text('i = —'), findsOneWidget);
      expect(find.textContaining('i = 3.50'), findsNothing);
    });

    testWidgets('null angle parks the pointer and shows a — badge', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(
          rotorDeg: null,
          outputDeg: 12.0,
          ratioLabel: '1.00',
        ),
      );

      final painter = _painter(tester);
      expect(painter.rotorDeg, isNull);
      expect(painter.outputDeg, 12);
      expect(
        find.byKey(const Key('encoder_dial_rotor_nodata')),
        findsOneWidget,
      );
      expect(find.byKey(const Key('encoder_dial_output_nodata')), findsNothing);
    });

    testWidgets('painter takes its colours from GuiPalette tokens', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '—'),
      );
      const palette = GuiPalette();
      final painter = _painter(tester);
      expect(painter.accent, palette.accent);
      expect(painter.line, palette.line);
      expect(painter.fgDim, palette.fgDim);
      expect(painter.valueAmber, palette.valueAmber);
      expect(EncoderSchematicPainter.strokeWidth, 1.2);
    });
  });

  group('EncoderSchematicPainter repaint', () {
    test('repaints when either angle or the ratio changes', () {
      const a = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 0,
        ratioLabel: '1.00',
        accent: Color(0xFF3F9E98),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
        valueAmber: Color(0xFFBFA84C),
      );
      final rotorMoved = EncoderSchematicPainter(
        rotorDeg: 90,
        outputDeg: 0,
        ratioLabel: '1.00',
        accent: a.accent,
        line: a.line,
        fgDim: a.fgDim,
        valueAmber: a.valueAmber,
      );
      final outputMoved = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 45,
        ratioLabel: '1.00',
        accent: a.accent,
        line: a.line,
        fgDim: a.fgDim,
        valueAmber: a.valueAmber,
      );
      final ratioMoved = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 0,
        ratioLabel: '2.00',
        accent: a.accent,
        line: a.line,
        fgDim: a.fgDim,
        valueAmber: a.valueAmber,
      );
      expect(a.shouldRepaint(rotorMoved), isTrue);
      expect(a.shouldRepaint(outputMoved), isTrue);
      expect(a.shouldRepaint(ratioMoved), isTrue);
      expect(a.shouldRepaint(a), isFalse);
    });

    test('null angle vs parked 0° is a repaint', () {
      const parked = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 0,
        ratioLabel: '—',
        accent: Color(0xFF3F9E98),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
        valueAmber: Color(0xFFBFA84C),
      );
      const noData = EncoderSchematicPainter(
        rotorDeg: null,
        outputDeg: 0,
        ratioLabel: '—',
        accent: Color(0xFF3F9E98),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
        valueAmber: Color(0xFFBFA84C),
      );
      expect(parked.shouldRepaint(noData), isTrue);
    });
  });
}
