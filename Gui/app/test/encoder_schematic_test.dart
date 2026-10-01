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

/// The two spans of the `i = … : 1` annotation rich text.
List<TextSpan> _ratioSpans(WidgetTester tester) {
  final rich = tester.widget<Text>(find.byKey(EncoderSchematic.ratioLabelKey));
  return (rich.textSpan! as TextSpan).children!.cast<TextSpan>();
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
    test('unloaded / unparseable ratio falls back to a 12 / 18 pair', () {
      expect(gearTeethPair('—'), (12, 18));
      expect(gearTeethPair('abc'), (12, 18));
    });

    test('tooth counts derive from the ratio, the output grows with it', () {
      final (int evenIn, int evenOut) = gearTeethPair('1.00');
      expect(evenOut, evenIn);

      final (int twoIn, int twoOut) = gearTeethPair('2.00');
      expect(twoOut, greaterThan(twoIn));
      expect(twoOut / twoIn, closeTo(2.0, 0.6));

      final (int bigIn, int bigOut) = gearTeethPair('3.50');
      expect(bigOut, greaterThan(bigIn));
      expect(bigOut, 28);
    });

    test('every derived pair stays inside z1 9..18 / z2 12..28', () {
      for (final label in const <String>[
        '0.01',
        '0.5',
        '0.98',
        '1.0',
        '1.5',
        '3.5',
        '12',
        '99',
      ]) {
        final (int input, int output) = gearTeethPair(label);
        expect(input, inInclusiveRange(9, 18), reason: label);
        expect(output, inInclusiveRange(12, 28), reason: label);
      }
    });
  });

  group('gear geometry', () {
    const size = Size(700, 220);

    test('addendum sits outside, dedendum inside the pitch circle', () {
      expect(EncoderSchematicPainter.addendumFactor, greaterThan(1.0));
      expect(EncoderSchematicPainter.dedendumFactor, lessThan(1.0));

      final (double rIn, double rOut) = EncoderSchematicPainter.pitchRadiiFor(
        size,
        '2.00',
      );
      expect(rIn, greaterThan(0));
      expect(rOut, greaterThan(rIn));
      final double tip = EncoderSchematicPainter.gearTipRadiusFor(size, '2.00');
      expect(tip, greaterThan(rOut));
      expect(
        tip,
        lessThanOrEqualTo(EncoderSchematicPainter.gearRadius(size) + 0.001),
      );
      expect(
        EncoderSchematicPainter.pitchCircleWidth,
        lessThan(EncoderSchematicPainter.strokeWidth),
      );
      // 简化画法移除了减重孔与键槽；齿轮的识别性改由
      // 轮齿 + 轮毂 + 中心孔 + 节圆承担，这四样必须仍在。
      expect(EncoderSchematicPainter.hubFactor, greaterThan(0));
      expect(
        EncoderSchematicPainter.hubBoreFactor,
        lessThan(EncoderSchematicPainter.hubFactor),
      );
    });

    test(
      'pitch circles are tangent at the mesh point (distance = r1 + r2)',
      () {
        final (double xIn, double xOut) =
            EncoderSchematicPainter.gearCentersXFor(size, '2.00');
        final (double rIn, double rOut) = EncoderSchematicPainter.pitchRadiiFor(
          size,
          '2.00',
        );
        expect(xOut - xIn, closeTo(rIn + rOut, 1e-6));
      },
    );
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
        const EncoderSchematic(rotorDeg: 90, outputDeg: 15, ratioLabel: '1.00'),
      );
      final painter = _painter(tester);
      expect(painter.rotorDeg, 90);
      expect(painter.outputDeg, 15);
    });

    testWidgets('ratio prefix is fgDim and the value is amber', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '3.50'),
      );
      const palette = GuiPalette();
      final spans = _ratioSpans(tester);
      expect(spans[0].text, 'i = ');
      expect(spans[1].text, '3.50 : 1');
      expect(spans[0].style!.color, palette.fgDim);
      expect(spans[1].style!.color, palette.valueAmber);
    });

    testWidgets('null ratio renders i = — with a muted value', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '—'),
      );
      const palette = GuiPalette();
      final spans = _ratioSpans(tester);
      expect(spans[0].text, 'i = ');
      expect(spans[1].text, '—');
      expect(spans[1].style!.color, palette.fgMute);
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
      expect(painter.bgBase, palette.bgBase);
      expect(painter.glowSoft, palette.glowSoft);
      expect(EncoderSchematicPainter.strokeWidth, 1.2);
    });

    testWidgets('input and output wheels carry distinct fill + rim colours', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '1.00'),
      );
      const palette = GuiPalette();
      final painter = _painter(tester);
      expect(painter.gearGreen, palette.gearGreen);
      expect(painter.gearGreenRim, palette.gearGreenRim);
      expect(painter.gearViolet, palette.gearViolet);
      expect(painter.gearVioletRim, palette.gearVioletRim);
      expect(painter.gearGreen, isNot(painter.gearViolet));
      expect(painter.gearGreenRim, isNot(painter.gearVioletRim));
      expect(painter.gearGreenRim, isNot(painter.gearGreen));
      expect(painter.gearVioletRim, isNot(painter.gearViolet));
    });
  });

  group('gear rotation animation', () {
    test('coupled output angle reverses and scales by the tooth ratio', () {
      for (final label in const <String>['1.00', '2.00', '3.50']) {
        final (int zIn, int zOut) = gearTeethPair(label);
        expect(
          coupledOutputGearAngleDeg(137.5, zIn, zOut),
          closeTo(-137.5 * zIn / zOut, 1e-6),
          reason: label,
        );
      }
    });

    testWidgets('转子正转 → 齿轮正转；转子反转 → 齿轮反转', (tester) async {
      // 新契约：齿轮转动由**转子读数**驱动，不再是恒速装饰循环。
      // 因此它必须跟随真实方向 —— 这正是「正反转要符合」的要求。
      //
      // 显示角带 lerp 平滑，单帧仍在追赶旧目标，故每段都喂多次读数
      // 让它收敛后再比较方向。
      Future<void> drive(int fromDeg, int toDeg, int steps) async {
        final int span = toDeg - fromDeg;
        for (var i = 0; i <= steps; i++) {
          final int deg = fromDeg + (span * i / steps).round();
          await _pump(
            tester,
            EncoderSchematic(
              rotorDeg: deg.toDouble(),
              outputDeg: 0,
              ratioLabel: '1.00',
            ),
          );
          await tester.pump(const Duration(milliseconds: 50));
        }
      }

      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '1.00'),
      );
      final start = _painter(tester).inputAngleDeg;

      await drive(0, 240, 12); // 正转 240°
      final forward = _painter(tester).inputAngleDeg;
      expect(forward, greaterThan(start), reason: '转子正转时齿轮应正转');

      await drive(240, 0, 12); // 反转回 0
      expect(
        _painter(tester).inputAngleDeg,
        lessThan(forward),
        reason: '转子反转时齿轮应反转',
      );
    });

    testWidgets('读数不变则齿轮不空转（电机没转，齿轮就不该转）', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 42, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 200));
      final settled = _painter(tester).inputAngleDeg;
      await tester.pump(const Duration(milliseconds: 500));
      expect(
        _painter(tester).inputAngleDeg,
        closeTo(settled, 1e-9),
        reason: '读数恒定 = 电机静止，齿轮必须停住（旧实现会恒速空转）',
      );
    });

    testWidgets('null rotor honest-stops the gears', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(
          rotorDeg: null,
          outputDeg: 0,
          ratioLabel: '1.00',
        ),
      );
      final before = _painter(tester).inputAngleDeg;
      await tester.pump(const Duration(milliseconds: 500));
      expect(_painter(tester).inputAngleDeg, closeTo(before, 1e-9));
    });

    testWidgets('the gear pair keeps θ_out = −θ_in · z1/z2 in flight', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '2.00'),
      );
      await tester.pump(const Duration(milliseconds: 500));
      final painter = _painter(tester);
      final (int zIn, int zOut) = gearTeethPair('2.00');
      expect(
        painter.outputGearAngleDeg,
        closeTo(-painter.inputAngleDeg * zIn / zOut, 1e-6),
      );
    });

    testWidgets('recovering live resumes from the frozen phase (no jump)', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(
          rotorDeg: null,
          outputDeg: 0,
          ratioLabel: '1.00',
        ),
      );
      await tester.pump(const Duration(milliseconds: 500));
      final frozen = _painter(tester).inputAngleDeg;
      expect(frozen, closeTo(0, 1e-9));

      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '1.00'),
      );
      // 恢复瞬间相位不跳变。
      expect(_painter(tester).inputAngleDeg, closeTo(frozen, 1e-6));

      // 恢复后**转子开始转动**（喂递增读数），齿轮才随之继续；
      // 恒定的读数意味着电机没动，齿轮也不该动。
      for (var i = 1; i <= 8; i++) {
        await _pump(
          tester,
          EncoderSchematic(
            rotorDeg: (i * 20).toDouble(),
            outputDeg: 0,
            ratioLabel: '1.00',
          ),
        );
        await tester.pump(const Duration(milliseconds: 50));
      }
      expect(_painter(tester).inputAngleDeg, isNot(closeTo(frozen, 1e-9)));
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
        gearGreen: Color(0xFF589E5E),
        gearGreenRim: Color(0xFF3C6E44),
        gearViolet: Color(0xFF8878B8),
        gearVioletRim: Color(0xFF5E5288),
        bgBase: Color(0xFF081018),
        glowSoft: Color(0x1F3F9E98),
      );
      final rotorMoved = EncoderSchematicPainter(
        rotorDeg: 90,
        outputDeg: 0,
        ratioLabel: '1.00',
        accent: a.accent,
        line: a.line,
        fgDim: a.fgDim,
        valueAmber: a.valueAmber,
        gearGreen: a.gearGreen,
        gearGreenRim: a.gearGreenRim,
        gearViolet: a.gearViolet,
        gearVioletRim: a.gearVioletRim,
        bgBase: a.bgBase,
        glowSoft: a.glowSoft,
      );
      final outputMoved = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 45,
        ratioLabel: '1.00',
        accent: a.accent,
        line: a.line,
        fgDim: a.fgDim,
        valueAmber: a.valueAmber,
        gearGreen: a.gearGreen,
        gearGreenRim: a.gearGreenRim,
        gearViolet: a.gearViolet,
        gearVioletRim: a.gearVioletRim,
        bgBase: a.bgBase,
        glowSoft: a.glowSoft,
      );
      final ratioMoved = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 0,
        ratioLabel: '2.00',
        accent: a.accent,
        line: a.line,
        fgDim: a.fgDim,
        valueAmber: a.valueAmber,
        gearGreen: a.gearGreen,
        gearGreenRim: a.gearGreenRim,
        gearViolet: a.gearViolet,
        gearVioletRim: a.gearVioletRim,
        bgBase: a.bgBase,
        glowSoft: a.glowSoft,
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
        gearGreen: Color(0xFF589E5E),
        gearGreenRim: Color(0xFF3C6E44),
        gearViolet: Color(0xFF8878B8),
        gearVioletRim: Color(0xFF5E5288),
        bgBase: Color(0xFF081018),
        glowSoft: Color(0x1F3F9E98),
      );
      const noData = EncoderSchematicPainter(
        rotorDeg: null,
        outputDeg: 0,
        ratioLabel: '—',
        accent: Color(0xFF3F9E98),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
        valueAmber: Color(0xFFBFA84C),
        gearGreen: Color(0xFF589E5E),
        gearGreenRim: Color(0xFF3C6E44),
        gearViolet: Color(0xFF8878B8),
        gearVioletRim: Color(0xFF5E5288),
        bgBase: Color(0xFF081018),
        glowSoft: Color(0x1F3F9E98),
      );
      expect(parked.shouldRepaint(noData), isTrue);
    });

    test('a changed gear rotation phase is a repaint', () {
      const base = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 0,
        ratioLabel: '1.00',
        accent: Color(0xFF3F9E98),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
        valueAmber: Color(0xFFBFA84C),
        gearGreen: Color(0xFF589E5E),
        gearGreenRim: Color(0xFF3C6E44),
        gearViolet: Color(0xFF8878B8),
        gearVioletRim: Color(0xFF5E5288),
        bgBase: Color(0xFF081018),
        glowSoft: Color(0x1F3F9E98),
      );
      const turned = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 0,
        ratioLabel: '1.00',
        inputAngleDeg: 15,
        outputGearAngleDeg: -10,
        accent: Color(0xFF3F9E98),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
        valueAmber: Color(0xFFBFA84C),
        gearGreen: Color(0xFF589E5E),
        gearGreenRim: Color(0xFF3C6E44),
        gearViolet: Color(0xFF8878B8),
        gearVioletRim: Color(0xFF5E5288),
        bgBase: Color(0xFF081018),
        glowSoft: Color(0x1F3F9E98),
      );
      expect(base.shouldRepaint(turned), isTrue);
      expect(base.shouldRepaint(base), isFalse);
    });
  });

  group('pointer perceptual scaling', () {
    EncoderSchematicState state(WidgetTester tester) =>
        tester.state<EncoderSchematicState>(find.byType(EncoderSchematic));

    test('scale constants', () {
      expect(kPointerSpeedScale, 0.05);
      expect(kPointerMaxDps, 180.0);
    });

    testWidgets('first live reading snaps the display pointer to measured', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 90, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 16));
      expect(state(tester).displayRotorDeg, closeTo(90, 1e-6));
    });

    testWidgets('measured +90° advances forward, clamped by kPointerMaxDps', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 16));

      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 90, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 16));

      final advanced = state(tester).displayRotorDeg!;
      expect(advanced, greaterThan(0));
      expect(advanced, lessThanOrEqualTo(kPointerMaxDps * 0.016 + 1e-6));
      expect(advanced, lessThan(90 * kPointerSpeedScale));
    });

    testWidgets('direction follows the measured sign (shortest arc)', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 16));

      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: -90, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 16));

      expect(state(tester).displayRotorDeg, lessThan(0));
    });

    testWidgets('a +360° sweep keeps advancing forward without a jump', (
      tester,
    ) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 0, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 16));

      var previous = state(tester).displayRotorDeg!;
      for (final measured in const <double>[90, 180, 270, 360]) {
        await _pump(
          tester,
          EncoderSchematic(
            rotorDeg: measured,
            outputDeg: 0,
            ratioLabel: '1.00',
          ),
        );
        await tester.pump(const Duration(milliseconds: 16));
        final now = state(tester).displayRotorDeg!;
        expect(now, greaterThanOrEqualTo(previous));
        expect(now - previous, lessThanOrEqualTo(kPointerMaxDps * 0.016 + 1e-6));
        previous = now;
      }
      expect(previous, greaterThan(0));
    });

    testWidgets('null reading freezes the display pointer', (tester) async {
      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: 45, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 16));
      final frozen = state(tester).displayRotorDeg;
      expect(frozen, isNotNull);

      await _pump(
        tester,
        const EncoderSchematic(rotorDeg: null, outputDeg: 0, ratioLabel: '1.00'),
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(state(tester).displayRotorDeg, frozen);
    });
  });
}
