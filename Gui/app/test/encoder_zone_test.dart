import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/bridge/frb/api.dart' show FrbError;
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

/// Full plain text of a `Text.rich`/`Text` widget identified by [key].
String _richText(WidgetTester tester, Key key) {
  final rich = tester.widget<Text>(find.byKey(key));
  return (rich.textSpan! as TextSpan).toPlainText();
}

/// Full `i = … : 1` text of the schematic's ratio annotation rich text.
String _ratioFullText(WidgetTester tester) =>
    _richText(tester, EncoderSchematic.ratioLabelKey);

/// Full `i = … : 1` text of the data-area reduction-ratio row.
String _reductionFullText(WidgetTester tester) =>
    _richText(tester, const Key('encoder_reduction_ratio_value'));

/// Seeds the two gear-ratio parameters decoded from raw f32.
void _seedRatios(AppState state, {double? rotor, double? output}) {
  if (rotor != null) {
    state.values['motor.encoder.rotor_ratio'] = ParamValueDto(
      ty: 0,
      raw: f32(rotor),
    );
  }
  if (output != null) {
    state.values['motor.encoder.output_ratio'] = ParamValueDto(
      ty: 0,
      raw: f32(output),
    );
  }
}

void main() {
  group('EncoderZone 面板', () {
    testWidgets('面板存在：标题「编码器 / ENCODERS」+ HairlineFrame main', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      expect(find.text('编码器'), findsOneWidget);
      expect(find.text('ENCODERS'), findsOneWidget);
      expect(find.text('双编码器'), findsNothing);
      expect(find.text('DUAL ENCODERS'), findsNothing);
      final frame = tester.widget<HairlineFrame>(
        find.byKey(const Key('encoder_frame')),
      );
      expect(frame.variant, HairlineVariant.main);
    });

    testWidgets('两栏标题「转子编码器」「出轴编码器」，角度各 1 位小数 °', (tester) async {
      final state = AppState.test(
        status: makeStatus(rotor: 123.456, output: 12.34),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      expect(find.text('转子编码器'), findsOneWidget);
      expect(find.text('出轴编码器'), findsOneWidget);
      expect(find.text('123.5'), findsOneWidget);
      expect(find.text('12.3'), findsOneWidget);
      // Unit `°` is rendered once per encoder reading.
      expect(find.text('°'), findsNWidgets(2));
      // Both encoders are live in a freshly seeded status: two `✓ 正常`.
      expect(find.text('正常'), findsNWidgets(2));
      expect(find.byType(StatusBadge), findsNWidgets(2));
    });

    testWidgets('数据列角度按量纲着色（°→cyan），栏标题统一青色', (tester) async {
      final state = AppState.test(
        status: makeStatus(rotor: 123.456, output: 12.34),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      final title = tester.widget<Text>(find.text('转子编码器'));
      expect(title.style?.color, const GuiPalette().accent);
      expect(title.style?.shadows, isNotEmpty);
      final angle = tester.widget<Text>(find.text('123.5'));
      expect(angle.style?.color, const GuiPalette().valueCyan);
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

  group('EncoderZone 减速比计算', () {
    testWidgets('缺省（两参均缺）：数据行与示意图均 i = —', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      expect(_reductionFullText(tester), 'i = —');
      expect(_ratioFullText(tester), 'i = —');
    });

    testWidgets('载入：转子 1.000 / 出轴 0.980 → i = 1.02 : 1', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      _seedRatios(state, rotor: 1.0, output: 0.98);

      await _pump(tester, EncoderZone(state: state));

      expect(_reductionFullText(tester), 'i = 1.02 : 1');
      expect(_ratioFullText(tester), 'i = 1.02 : 1');
    });

    testWidgets('除零：出轴齿比 0 → i = —（不产生 Inf/NaN）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      _seedRatios(state, rotor: 1.0, output: 0.0);

      await _pump(tester, EncoderZone(state: state));

      expect(_reductionFullText(tester), 'i = —');
      expect(_ratioFullText(tester), 'i = —');
    });

    testWidgets('缺出轴参：转子 2.000 / 出轴缺 → i = —', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      _seedRatios(state, rotor: 2.0);

      await _pump(tester, EncoderZone(state: state));

      expect(_reductionFullText(tester), 'i = —');
    });

    testWidgets('减速比数值用 valueMagenta 强调，前缀 fgDim', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      _seedRatios(state, rotor: 1.0, output: 0.98);

      await _pump(tester, EncoderZone(state: state));

      final rich = tester.widget<Text>(
        find.byKey(const Key('encoder_reduction_ratio_value')),
      );
      final spans = (rich.textSpan! as TextSpan).children!.cast<TextSpan>();
      const palette = GuiPalette();
      expect(spans[0].text, 'i = ');
      expect(spans[0].style!.color, palette.fgDim);
      expect(spans[1].text, '1.02 : 1');
      expect(spans[1].style!.color, palette.valueMagenta);
    });
  });

  group('EncoderZone 左右分列', () {
    testWidgets('转子列在左、出轴列在右，角度与转速分列', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      final rotorColumn = find.byKey(const Key('encoder_column_rotor'));
      final outputColumn = find.byKey(const Key('encoder_column_output'));
      expect(rotorColumn, findsOneWidget);
      expect(outputColumn, findsOneWidget);

      // 左列：转子角度 + 转子转速。
      expect(
        find.descendant(
          of: rotorColumn,
          matching: find.byKey(const Key('encoder_row_rotor')),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: rotorColumn,
          matching: find.byKey(const Key('encoder_rpm_rotor')),
        ),
        findsOneWidget,
      );
      // 右列：出轴角度 + 出轴转速。
      expect(
        find.descendant(
          of: outputColumn,
          matching: find.byKey(const Key('encoder_row_output')),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: outputColumn,
          matching: find.byKey(const Key('encoder_rpm_output')),
        ),
        findsOneWidget,
      );

      // 两列水平分离：转子列整体位于出轴列左侧。
      expect(
        tester.getTopLeft(rotorColumn).dx,
        lessThan(tester.getTopLeft(outputColumn).dx),
      );
    });
  });

  group('EncoderZone 两栏标题 / 对齐 / 分割线', () {
    testWidgets('标题紧邻状态徽标（三重编码）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      expect(
        find.descendant(
          of: find.byKey(const Key('encoder_header_rotor')),
          matching: find.byType(StatusBadge),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: find.byKey(const Key('encoder_header_output')),
          matching: find.byType(StatusBadge),
        ),
        findsOneWidget,
      );
      // 徽标不再逐行重复：整幅仅两枚（每栏标题各一）。
      expect(find.byType(StatusBadge), findsNWidgets(2));
      // 标题徽标紧邻其列标题文本。
      expect(
        find.descendant(
          of: find.byKey(const Key('encoder_header_rotor')),
          matching: find.text('转子编码器'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: find.byKey(const Key('encoder_header_output')),
          matching: find.text('出轴编码器'),
        ),
        findsOneWidget,
      );
    });

    testWidgets('角度/转速两行跨栏同行对齐', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      final rotorAngle = tester
          .getTopLeft(find.byKey(const Key('encoder_row_rotor')))
          .dy;
      final outputAngle = tester
          .getTopLeft(find.byKey(const Key('encoder_row_output')))
          .dy;
      expect(rotorAngle, closeTo(outputAngle, 0.5));

      final rotorRpm = tester
          .getTopLeft(find.byKey(const Key('encoder_rpm_rotor')))
          .dy;
      final outputRpm = tester
          .getTopLeft(find.byKey(const Key('encoder_rpm_output')))
          .dy;
      expect(rotorRpm, closeTo(outputRpm, 0.5));
      expect(rotorAngle, lessThan(rotorRpm));
    });

    testWidgets('中央竖分割线：1px lineDim，位于两栏之间', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      final divider = find.byKey(const Key('encoder_column_divider'));
      expect(divider, findsOneWidget);
      expect(tester.getSize(divider).width, 1);
      final container = tester.widget<Container>(divider);
      expect(container.color, const GuiPalette().lineDim);

      final rotorColumn = find.byKey(const Key('encoder_column_rotor'));
      final outputColumn = find.byKey(const Key('encoder_column_output'));
      final dx = tester.getCenter(divider).dx;
      expect(dx, greaterThan(tester.getTopLeft(rotorColumn).dx));
      expect(dx, lessThan(tester.getTopRight(outputColumn).dx));
    });

    testWidgets('出轴零点按钮组保留在右栏', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      final outputColumn = find.byKey(const Key('encoder_column_output'));
      expect(
        find.descendant(
          of: outputColumn,
          matching: find.byKey(const Key('encoder_zero_set_now')),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: outputColumn,
          matching: find.byKey(const Key('encoder_zero_set_abs')),
        ),
        findsOneWidget,
      );
      // 转子零点归属提示随按钮同栏。
      expect(
        find.descendant(
          of: outputColumn,
          matching: find.textContaining('电机·驱动'),
        ),
        findsOneWidget,
      );
    });
  });

  group('EncoderZone 出轴零点快捷设置', () {
    testWidgets('按钮 A「当前角设零点」→ setOutputZeroNow + 成功 toast', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, EncoderZone(state: state));

      await tester.tap(find.byKey(const Key('encoder_zero_set_now')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(ops, <String>['setOutputZeroNow']);
      expect(
        find.descendant(
          of: find.byKey(const Key('gui_toast_frame')),
          matching: find.text('出轴零点已设置'),
        ),
        findsOneWidget,
      );
      // 示意图 ticker 常驻，不能用 pumpAndSettle；按固定帧推进冲掉 toast。
      await tester.pump(const Duration(milliseconds: 3000));
    });

    testWidgets('按钮 B「指定角度设零点」→ 对话框输入 45 → setOutputZeroAbs', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, EncoderZone(state: state));

      await tester.tap(find.byKey(const Key('encoder_zero_set_abs')));
      // 示意图 ticker 常驻（live 读数），不能用 pumpAndSettle；按固定帧推进。
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));
      expect(find.byKey(const Key('encoder_zero_angle_field')), findsOneWidget);

      await tester.enterText(
        find.byKey(const Key('encoder_zero_angle_field')),
        '45',
      );
      await tester.tap(find.byKey(const Key('encoder_zero_angle_confirm')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));

      expect(ops, <String>['setOutputZeroAbs']);
      expect(
        find.descendant(
          of: find.byKey(const Key('gui_toast_frame')),
          matching: find.text('出轴零点已设置'),
        ),
        findsOneWidget,
      );
      await tester.pump(const Duration(milliseconds: 3000));
    });

    testWidgets('按钮 B 越界输入被拒（−360..360），不下发', (tester) async {
      final ops = <String>[];
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => ops.add(op);

      await _pump(tester, EncoderZone(state: state));

      await tester.tap(find.byKey(const Key('encoder_zero_set_abs')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));
      await tester.enterText(
        find.byKey(const Key('encoder_zero_angle_field')),
        '400',
      );
      await tester.tap(find.byKey(const Key('encoder_zero_angle_confirm')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));

      expect(ops, isEmpty);
      // 对话框保持打开并给出范围提示。
      expect(find.byKey(const Key('encoder_zero_angle_field')), findsOneWidget);
      expect(find.textContaining('−360'), findsOneWidget);
    });

    testWidgets('联锁拒绝 code 4 → 既有文案表（FAULT_CLEAR / STOP 引导）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async =>
          throw const FrbError(code: 4, message: 'interlock');

      await _pump(tester, EncoderZone(state: state));

      await tester.tap(find.byKey(const Key('encoder_zero_set_now')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.textContaining('FAULT_CLEAR'), findsOneWidget);
      expect(find.textContaining('STOP'), findsOneWidget);
    });

    testWidgets('RANGE_ERR code 6 → 「设备判定越界」', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async =>
          throw const FrbError(code: 6, message: 'range');

      await _pump(tester, EncoderZone(state: state));

      await tester.tap(find.byKey(const Key('encoder_zero_set_now')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('设备判定越界'), findsOneWidget);
    });

    testWidgets('转子零点归属提示：注明由 电机·驱动 负责', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      expect(find.textContaining('电机·驱动'), findsOneWidget);
    });
  });

  group('EncoderZone 分辨率', () {
    testWidgets('分辨率缺省：显示 —', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      expect(
        find.descendant(
          of: find.byKey(const Key('encoder_resolution')),
          matching: find.text('—'),
        ),
        findsOneWidget,
      );
    });

    testWidgets('分辨率载入：显示 14 bit', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.values['motor.encoder.resolution_bits'] = ParamValueDto(
        ty: 1,
        raw: 14,
      );

      await _pump(tester, EncoderZone(state: state));

      expect(
        find.descendant(
          of: find.byKey(const Key('encoder_resolution')),
          matching: find.text('14 bit'),
        ),
        findsOneWidget,
      );
    });
  });

  group('EncoderSchematic', () {
    testWidgets('双刻度盘指针均为 valueMagenta，盘圈保持青', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      _seedRatios(state, rotor: 1.0, output: 0.98);
      const palette = GuiPalette();

      await _pump(tester, EncoderZone(state: state));

      expect(find.byKey(const Key('encoder_schematic')), findsOneWidget);
      final paint = tester.widget<CustomPaint>(_schematicPaint());
      final painter = paint.painter! as EncoderSchematicPainter;
      expect(painter.pointerColor, palette.valueMagenta);
      expect(painter.accent, palette.accent);
      expect(EncoderSchematicPainter.strokeWidth, 1.2);

      // 减速比标注数值也取洋红（前缀 fgDim）。
      final rich = tester.widget<Text>(
        find.byKey(EncoderSchematic.ratioLabelKey),
      );
      final spans = (rich.textSpan! as TextSpan).children!.cast<TextSpan>();
      expect(spans[0].style!.color, palette.fgDim);
      expect(spans[1].style!.color, palette.valueMagenta);
    });

    test('painter 在线色变化时重绘，相同则否', () {
      const a = EncoderSchematicPainter(
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
        pointerColor: Color(0xFFE040C8),
      );
      const b = EncoderSchematicPainter(
        rotorDeg: 0,
        outputDeg: 0,
        ratioLabel: '—',
        accent: Color(0xFFFFFFFF),
        line: Color(0xFF1E5458),
        fgDim: Color(0xFF6E908E),
        valueAmber: Color(0xFFBFA84C),
        gearGreen: Color(0xFF589E5E),
        gearGreenRim: Color(0xFF3C6E44),
        gearViolet: Color(0xFF8878B8),
        gearVioletRim: Color(0xFF5E5288),
        bgBase: Color(0xFF081018),
        glowSoft: Color(0x1F3F9E98),
        pointerColor: Color(0xFFE040C8),
      );
      expect(a.shouldRepaint(b), isTrue);
      expect(a.shouldRepaint(a), isFalse);
    });

    test('指针色变化触发重绘', () {
      const a = EncoderSchematicPainter(
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
        pointerColor: Color(0xFFE040C8),
      );
      const b = EncoderSchematicPainter(
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
        pointerColor: Color(0xFF00FF00),
      );
      expect(a.shouldRepaint(b), isTrue);
    });
  });
}
