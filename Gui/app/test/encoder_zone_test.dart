import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/encoder_zone.dart';
import 'package:g6618_gui/widgets/encoder_schematic.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/status_badge.dart';

StatusDto makeStatus({double rotor = 0, double output = 0}) => StatusDto(
  faultFlags: 0,
  motorRunning: false,
  motorState: 0,
  freqHz: 0,
  modPct: 0,
  iU: 0,
  iV: 0,
  iW: 0,
  vbus: 0,
  ntcTemp: 0,
  encRotorDeg: rotor,
  encOutputDeg: output,
);

int f32(double v) =>
    (ByteData(4)..setFloat32(0, v, Endian.little)).getInt32(0, Endian.little);

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

Finder _schematicPaint() => find.byWidgetPredicate(
  (w) => w is CustomPaint && w.painter is EncoderSchematicPainter,
);

void main() {
  group('EncoderZone 面板', () {
    testWidgets('面板存在：标题「双编码器 / DUAL ENCODERS」+ HairlineFrame main', (
      tester,
    ) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      expect(find.text('双编码器'), findsOneWidget);
      expect(find.text('DUAL ENCODERS'), findsOneWidget);
      final frame = tester.widget<HairlineFrame>(
        find.byKey(const Key('encoder_frame')),
      );
      expect(frame.variant, HairlineVariant.main);
    });

    testWidgets('两角度值渲染：ENC R / ENC O 各 1 位小数 °，徽标为正常', (tester) async {
      final state = AppState.test(
        status: makeStatus(rotor: 123.456, output: 12.34),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      expect(find.text('ENC R'), findsOneWidget);
      expect(find.text('ENC O'), findsOneWidget);
      expect(find.text('123.5'), findsOneWidget);
      expect(find.text('12.3'), findsOneWidget);
      // Unit `°` is rendered once per encoder reading.
      expect(find.text('°'), findsNWidgets(2));
      // Both encoders are live in a freshly seeded status: two `✓ 正常`.
      expect(find.text('正常'), findsNWidgets(2));
      expect(find.byType(StatusBadge), findsNWidgets(2));
    });

    testWidgets('stale：角度降级为 — 并显示失效徽标', (tester) async {
      final state = AppState.test(
        status: makeStatus(rotor: 10, output: 20),
        metas: const [],
      );
      addTearDown(state.dispose);
      state.seedFreshness(
        AppState.statusTileKey('ENC_R'),
        DateTime.now().subtract(const Duration(seconds: 2)),
      );

      await _pump(tester, EncoderZone(state: state));

      final rotor = find.byKey(const Key('encoder_row_rotor'));
      expect(
        find.descendant(of: rotor, matching: find.text('—')),
        findsOneWidget,
      );
      expect(find.text('失效'), findsOneWidget);
      expect(find.text('10.0'), findsNothing);
    });
  });

  group('EncoderZone 齿比与无源项', () {
    testWidgets('齿比缺省：两行与示意图均显示 —', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      for (final key in const <String>[
        'encoder_ratio_rotor',
        'encoder_ratio_output',
      ]) {
        expect(
          find.descendant(of: find.byKey(Key(key)), matching: find.text('—')),
          findsOneWidget,
          reason: key,
        );
      }
      expect(find.text('×—'), findsOneWidget);
    });

    testWidgets('齿比载入：转子 1.000 / 出轴 0.980，示意图标 ×0.980', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.values['motor.encoder.rotor_ratio'] = ParamValueDto(
        ty: 0,
        raw: f32(1.0),
      );
      state.values['motor.encoder.output_ratio'] = ParamValueDto(
        ty: 0,
        raw: f32(0.98),
      );

      await _pump(tester, EncoderZone(state: state));

      expect(find.text('1.000'), findsOneWidget);
      expect(find.text('0.980'), findsOneWidget);
      expect(find.text('×0.980'), findsOneWidget);
    });

    testWidgets('零点 / 分辨率协议无源：一律显示 —', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      for (final key in const <String>[
        'encoder_zero',
        'encoder_resolution',
      ]) {
        expect(
          find.descendant(of: find.byKey(Key(key)), matching: find.text('—')),
          findsOneWidget,
          reason: key,
        );
      }
    });
  });

  group('EncoderSchematic', () {
    testWidgets('示意图 painter 在场，线色取自 GuiPalette token', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      const palette = GuiPalette();

      await _pump(tester, EncoderZone(state: state));

      expect(find.byKey(const Key('encoder_schematic')), findsOneWidget);
      final paint = tester.widget<CustomPaint>(_schematicPaint());
      final painter = paint.painter! as EncoderSchematicPainter;
      expect(painter.accent, palette.accent);
      expect(painter.line, palette.line);
      expect(painter.fgDim, palette.fgDim);
      expect(EncoderSchematicPainter.strokeWidth, 1.2);
    });

    test('painter 在线色变化时重绘，相同则否', () {
      const a = EncoderSchematicPainter(
        accent: Color(0xFF3F9E98),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
      );
      const b = EncoderSchematicPainter(
        accent: Color(0xFFFFFFFF),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
      );
      expect(a.shouldRepaint(b), isTrue);
      expect(a.shouldRepaint(a), isFalse);
    });
  });
}
