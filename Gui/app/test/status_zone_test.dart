import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/scope_zone.dart';
import 'package:g6618_gui/ui/status_zone.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';

/// Bridge-like failure carrying a device error [code] (matches the old
/// dashboard test's fake so the interlock classification path is exercised).
class FrbLikeError implements Exception {
  final int code;
  const FrbLikeError({required this.code});

  @override
  String toString() => 'FrbLikeError(code: $code)';
}

StatusDto makeStatus({
  int faultFlags = 0,
  bool motorRunning = false,
  int motorState = 0,
  double vbus = 0,
  double freq = 0,
  double mod = 0,
  double iU = 0,
  double iV = 0,
  double iW = 0,
  double ntc = 0,
  double rotor = 0,
  double output = 0,
}) => StatusDto(
  faultFlags: faultFlags,
  motorRunning: motorRunning,
  motorState: motorState,
  freqHz: freq,
  modPct: mod,
  iU: iU,
  iV: iV,
  iW: iW,
  vbus: vbus,
  ntcTemp: ntc,
  encRotorDeg: rotor,
  encOutputDeg: output,
);

int f32(double v) =>
    (ByteData(4)..setFloat32(0, v, Endian.little)).getInt32(0, Endian.little);

Future<void> _pump(WidgetTester tester, Widget child) {
  tester.view.physicalSize = const Size(1400, 1400);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(body: SingleChildScrollView(child: child)),
    ),
  );
}

void main() {
  group('阈值 getter', () {
    test('按名称从已加载参数读取，缺失返回 null', () {
      final state = AppState.test();
      addTearDown(state.dispose);
      state.values['software.fault.vbus_ov_v'] = ParamValueDto(
        ty: 0,
        raw: f32(36.0),
      );
      // Simulated sample: the firmware does not expose `ntc_ot_c` (the NTC
      // over-temperature bit is reserved), so this only exercises the decode
      // path — it is not a real device parameter.
      state.values['software.fault.ntc_ot_c'] = ParamValueDto(
        ty: 0,
        raw: f32(85.0),
      );
      state.values['motor.i_peak_10s_a'] = ParamValueDto(ty: 0, raw: f32(24.3));

      expect(state.thresholdVbus, closeTo(36.0, 1e-3));
      expect(state.thresholdNtc, closeTo(85.0, 1e-3));
      expect(state.thresholdCurrent, closeTo(24.3, 1e-3));
    });

    test('thresholdCurrent 优先 motor.i_peak_10s_a，缺省回退 oc_trip_a', () {
      final state = AppState.test();
      addTearDown(state.dispose);
      state.values['software.fault.oc_trip_a'] = ParamValueDto(
        ty: 0,
        raw: f32(72.9),
      );
      expect(state.thresholdCurrent, closeTo(72.9, 1e-3));
      expect(state.thresholdNtc, isNull);
      expect(state.thresholdVbus, isNull);
    });
  });

  group('StatusTile 三态与精度', () {
    testWidgets('按量纲渲染精度：V 1 位 / A 3 位 / Hz 2 位', (tester) async {
      final state = AppState.test(
        status: makeStatus(vbus: 24.0, iU: 0.0, freq: 50.0),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, StatusZone(state: state));

      expect(
        find.descendant(
          of: find.byKey(const Key('status_tile_VBUS')),
          matching: find.text('24.0'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: find.byKey(const Key('status_tile_I_U')),
          matching: find.text('0.000'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: find.byKey(const Key('status_tile_F_ELEC')),
          matching: find.text('50.00'),
        ),
        findsOneWidget,
      );
    });

    testWidgets('stale 降级为 — + 虚线，并显示 N 秒未更新', (tester) async {
      await _pump(
        tester,
        const StatusTile(
          label: 'I_V',
          zh: 'V 相电流',
          value: 0.0,
          unit: 'A',
          precision: 3,
          note: 'RMS',
          freshness: Freshness.stale,
          staleSeconds: 1.5,
        ),
      );

      expect(find.text('— A'), findsOneWidget);
      expect(find.byType(DashedFrame), findsOneWidget);
      expect(find.textContaining('未更新'), findsOneWidget);
    });

    testWidgets('unknown 显示裸 —（弱色虚线，无警告、不带单位）', (tester) async {
      await _pump(
        tester,
        const StatusTile(
          label: 'I_V',
          zh: 'V 相电流',
          value: null,
          unit: 'A',
          precision: 3,
          freshness: Freshness.unknown,
        ),
      );

      expect(find.text('—'), findsOneWidget);
      // A never-read value is not a failed poll: no stale/warn wording.
      expect(find.textContaining('未更新'), findsNothing);
      final frame = tester.widget<DashedFrame>(find.byType(DashedFrame));
      expect(frame.color, const GuiPalette().fgMute);
    });

    testWidgets('warn 值黄 + 接近门限（同色辉光）', (tester) async {
      await _pump(
        tester,
        const StatusTile(
          label: 'NTC',
          zh: '温度',
          value: 78.4,
          unit: '℃',
          precision: 1,
          note: 'NOMINAL 85',
          warn: true,
          warnText: '接近门限 85',
        ),
      );

      expect(find.text('78.4'), findsOneWidget);
      // Triple encoding: colour + icon + text (design-system §6 rule 1).
      expect(find.text('▲ 接近门限 85'), findsOneWidget);
      final value = tester.widget<Text>(find.text('78.4'));
      expect(value.style?.color, const GuiPalette().stWarn);
    });
  });

  group('ScopeZone 控制条与故障构成', () {
    testWidgets('控制失败 code 4 保留联锁 SnackBar 文案', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => throw const FrbLikeError(code: 4);

      await _pump(tester, ScopeZone(state: state));
      await tester.tap(find.text('启动'));
      await tester.pumpAndSettle();

      final snack = find.byType(SnackBar);
      expect(
        find.descendant(of: snack, matching: find.textContaining('联锁')),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: snack,
          matching: find.textContaining('FAULT_CLEAR'),
        ),
        findsOneWidget,
      );
    });

    testWidgets('故障位按 app_fault.h 语义出条目', (tester) async {
      final state = AppState.test(
        status: makeStatus(faultFlags: 0x1),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(find.textContaining('过流'), findsWidgets);
      expect(find.textContaining('U 相'), findsOneWidget);
      expect(find.text('CURRENT'), findsOneWidget);
    });

    testWidgets('锁存位标题为 LATCHED，不冒充 CURRENT', (tester) async {
      final state = AppState.test(
        status: makeStatus(faultFlags: 0x1),
        metas: const [],
      );
      addTearDown(state.dispose);
      // Latched set takes precedence over the live fault mask.
      state.latchedFlags = 1 << 1;

      await _pump(tester, ScopeZone(state: state));

      expect(find.text('LATCHED'), findsOneWidget);
      expect(find.text('CURRENT'), findsNothing);
      expect(find.textContaining('V 相'), findsOneWidget);
    });

    testWidgets('无法解码的位显示 bit N (0x..)', (tester) async {
      final state = AppState.test(
        status: makeStatus(faultFlags: 1 << 20),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(find.textContaining('bit 20'), findsOneWidget);
    });

    testWidgets('无故障时不出条目', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(find.textContaining('过流'), findsNothing);
      expect(find.textContaining('bit '), findsNothing);
    });
  });

  group('faultBitLabel', () {
    test('已知位返回中文语义，未知位返回 hex 回退', () {
      expect(faultBitLabel(0), '过流 L1 · U 相');
      expect(faultBitLabel(6), '母线过压');
      expect(faultBitLabel(20), 'bit 20 (0x00100000)');
    });
  });
}
