import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/encoder_zone.dart';
import 'package:g6618_gui/ui/limit_zone.dart';
import 'package:g6618_gui/ui/status_zone.dart';

StatusDto makeStatus({
  int faultFlags = 0,
  double freq = 0,
  double mod = 0,
  double iU = 0,
  double iV = 0,
  double iW = 0,
  double vbus = 0,
  double ntc = 0,
  double rotor = 0,
  double output = 0,
}) => StatusDto(
  faultFlags: faultFlags,
  motorRunning: false,
  motorState: 0,
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

Future<void> _pump(WidgetTester tester, Widget child) {
  tester.view.physicalSize = const Size(1600, 1600);
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
  group('派生量公式（纯函数）', () {
    test('deriveIIn：对称三相 + mod=100 → 1.5 × 相电流 RMS', () {
      // Symmetric RMS 2 A on all phases → sqrt(3·2²/3) = 2 A.
      expect(deriveIIn(2, 2, 2, 100), closeTo(3.0, 1e-9));
      // Zero modulation → zero estimate; all-zero current → zero estimate.
      expect(deriveIIn(2, 2, 2, 0), 0);
      expect(deriveIIn(0, 0, 0, 100), 0);
      // Unbalanced set still uses the balanced RMS definition: √(25/3) ≈ 2.887.
      expect(deriveIIn(3, 4, 0, 100), closeTo(1.5 * math.sqrt(25 / 3), 1e-9));
    });

    test('deriveNtcStator / derivePIn：热估算与功率，clamp ≥ 0', () {
      // I_rms = 10 A → +8 °C over the PCB NTC.
      expect(deriveNtcStator(40, 10, 10, 10), closeTo(48.0, 1e-9));
      expect(deriveNtcStator(40, 0, 0, 0), closeTo(40.0, 1e-9));
      expect(deriveNtcStator(-5, 0, 0, 0), 0);
      expect(derivePIn(48, 1.5), closeTo(72.0, 1e-9));
      expect(derivePIn(-48, 1.5), 0);
    });

    test('unwrapDeltaDeg：跨 ±180 取最短弧', () {
      expect(unwrapDeltaDeg(170, -170), closeTo(20, 1e-9));
      expect(unwrapDeltaDeg(-170, 170), closeTo(-20, 1e-9));
      expect(unwrapDeltaDeg(0, 90), closeTo(90, 1e-9));
      expect(unwrapDeltaDeg(0, -90), closeTo(-90, 1e-9));
      expect(unwrapDeltaDeg(359, 1), closeTo(2, 1e-9));
    });

    test('deriveRpm：Δθ/360/Δt·60，非正间隔返回 null', () {
      expect(deriveRpm(0, 6, 0.01), closeTo(100.0, 1e-9));
      expect(deriveRpm(170, -170, 0.01), closeTo(333.333333, 1e-3));
      expect(deriveRpm(0, 6, 0), isNull);
      expect(deriveRpm(0, 6, -0.01), isNull);
    });
  });

  group('rpm 滑窗角差解算与 EMA', () {
    test('首样本前为 —（null）；次批起滑窗解算 + EMA(α=0.15)', () {
      final state = AppState.test(status: makeStatus());
      addTearDown(state.dispose);
      var now = DateTime(2026, 1, 1);
      state.clock = () => now;

      // Before any sample the estimate is honestly absent (rendered —).
      expect(state.rpmRotor, isNull, reason: '从未有有效样本 → —');
      expect(state.rpmOutput, isNull, reason: '从未有有效样本 → —');

      state.feedBatch(
        StatusBatchDto(
          samples: <StatusDto>[makeStatus(rotor: 0)],
          seq: BigInt.one,
        ),
      );
      expect(state.rpmRotor, isNull, reason: '首批样本单点，窗内不足两点 → 维持 null');

      now = now.add(const Duration(milliseconds: 10));
      state.feedBatch(
        StatusBatchDto(
          samples: <StatusDto>[makeStatus(rotor: 6)],
          seq: BigInt.two,
        ),
      );
      // Window 0°→6° over 10 ms = 100 rpm; first estimate adopted directly.
      expect(state.rpmRotor, closeTo(100.0, 1e-6));

      now = now.add(const Duration(milliseconds: 10));
      state.feedBatch(
        StatusBatchDto(
          samples: <StatusDto>[makeStatus(rotor: 9)],
          seq: BigInt.from(3),
        ),
      );
      // Window 0°→9° over 20 ms = 75 rpm; 100 + (75 − 100)·0.15 = 96.25.
      expect(state.rpmRotor, closeTo(96.25, 1e-6));
    });

    test('连续样本后插入 600ms 间隙：转速保持数值（非 null / 非 —）', () {
      final state = AppState.test(status: makeStatus());
      addTearDown(state.dispose);
      var now = DateTime(2026, 1, 1);
      state.clock = () => now;

      // Uniform 6° / 10 ms = 100 rpm.
      for (var i = 0; i < 12; i++) {
        state.feedBatch(
          StatusBatchDto(
            samples: <StatusDto>[makeStatus(rotor: i * 6.0)],
            seq: BigInt.from(i + 1),
          ),
        );
        now = now.add(const Duration(milliseconds: 10));
      }
      expect(state.rpmRotor, closeTo(100.0, 1e-6));

      // A 600 ms discontinuity (> kRpmMaxGap) must NOT blank the readout.
      now = now.add(const Duration(milliseconds: 600));
      state.feedBatch(
        StatusBatchDto(
          samples: <StatusDto>[makeStatus(rotor: 200)],
          seq: BigInt.from(20),
        ),
      );
      expect(state.rpmRotor, isNotNull, reason: '间隙后必须保持上次估计，禁清空');
      expect(state.rpmRotor, closeTo(100.0, 1e-6));
    });

    test('匀速角序列 → 转速收敛到期望值 ±2%', () {
      final state = AppState.test(status: makeStatus());
      addTearDown(state.dispose);
      var now = DateTime(2026, 1, 1);
      state.clock = () => now;

      // 9° per 5 ms = 1800 °/s = 300 rpm, wrapped through 360.
      for (var i = 0; i < 60; i++) {
        state.feedBatch(
          StatusBatchDto(
            samples: <StatusDto>[makeStatus(rotor: (i * 9.0) % 360)],
            seq: BigInt.from(i + 1),
          ),
        );
        now = now.add(const Duration(milliseconds: 5));
      }
      expect(
        state.rpmRotor,
        closeTo(300.0, 300.0 * 0.02),
        reason: '匀速序列收敛 ±2%',
      );
    });
  });

  group('状态监视 5 瓦片', () {
    testWidgets('只留 VBUS / I_IN / P_IN / NTC / NTC_STATOR', (tester) async {
      final state = AppState.test(
        status: makeStatus(vbus: 48, iU: 1, iV: 1, iW: 1, mod: 100, ntc: 30),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, StatusZone(state: state));

      for (final name in const <String>[
        'VBUS',
        'I_IN',
        'P_IN',
        'NTC',
        'NTC_STATOR',
      ]) {
        expect(
          find.byKey(Key('status_frame_$name')),
          findsOneWidget,
          reason: name,
        );
      }
      expect(find.byType(StatusTile), findsNWidgets(5));
      // Removed tiles: phase currents move to ScopeZone, F_ELEC follows them.
      for (final gone in const <String>['I_U', 'I_V', 'I_W', 'F_ELEC']) {
        expect(find.byKey(Key('status_frame_$gone')), findsNothing, reason: gone);
      }
    });

    testWidgets('EST 瓦片副注区分估算：相电流反推 / 热估算', (tester) async {
      final state = AppState.test(
        status: makeStatus(vbus: 48, iU: 2, iV: 2, iW: 2, mod: 100, ntc: 30),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, StatusZone(state: state));

      expect(find.text('EST · 相电流反推'), findsNWidgets(2));
      expect(find.text('EST · 热估算'), findsOneWidget);
      // Estimate values: I_IN = 3.000 A, P_IN = 144.0 W, stator = 30 + 0.32 ≈ 30.3.
      expect(
        find.descendant(
          of: find.byKey(const Key('status_tile_I_IN')),
          matching: find.text('3.000'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: find.byKey(const Key('status_tile_P_IN')),
          matching: find.text('144.0'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: find.byKey(const Key('status_tile_NTC_STATOR')),
          matching: find.text('30.3'),
        ),
        findsOneWidget,
      );
    });

    testWidgets('无 status：派生瓦片全部 —（诚实）', (tester) async {
      final state = AppState.test(metas: const []);
      addTearDown(state.dispose);

      expect(state.iInEst, isNull);
      expect(state.pInEst, isNull);
      expect(state.ntcStatorEst, isNull);

      await _pump(tester, StatusZone(state: state));

      for (final name in const <String>['I_IN', 'P_IN', 'NTC_STATOR']) {
        expect(
          find.descendant(
            of: find.byKey(Key('status_tile_$name')),
            matching: find.text('—'),
          ),
          findsOneWidget,
          reason: name,
        );
      }
    });
  });

  group('相电流移出示数区', () {
    testWidgets('门限区仅保留 F_ELEC；I_U/I_V/I_W 相电流框已移除', (tester) async {
      final state = AppState.test(
        status: makeStatus(iU: 1, iV: 2, iW: 3, freq: 50),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, LimitZone(state: state));

      final panel = find.byKey(const Key('limit_freq_panel'));
      expect(panel, findsOneWidget);
      expect(
        find.descendant(of: panel, matching: find.text('F_ELEC')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: panel, matching: find.text('SETPOINT')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: panel, matching: find.text('50.00')),
        findsOneWidget,
      );
      // The three phase rows moved into the trend chart readout.
      for (final gone in const <String>['I_U', 'I_V', 'I_W', '相电流']) {
        expect(find.text(gone), findsNothing, reason: gone);
      }
      // The frequency panel still sits beside the relative-threshold panel.
      expect(find.byKey(const Key('limit_heat_panel')), findsOneWidget);
    });
  });

  group('双编码器转速标注', () {
    testWidgets('两转速行存在且带 EST 副注（无样本 → —）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, EncoderZone(state: state));

      final rotor = find.byKey(const Key('encoder_rpm_rotor'));
      final output = find.byKey(const Key('encoder_rpm_output'));
      expect(rotor, findsOneWidget);
      expect(output, findsOneWidget);
      expect(
        find.descendant(of: rotor, matching: find.textContaining('角差分 EST')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: output, matching: find.textContaining('磁编')),
        findsOneWidget,
      );
      // No batch fed → estimates are honestly —.
      expect(
        find.descendant(of: rotor, matching: find.text('—')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: output, matching: find.text('—')),
        findsOneWidget,
      );
    });
  });
}
