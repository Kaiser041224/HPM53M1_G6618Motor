import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/scope_zone.dart';
import 'package:g6618_gui/ui/status_zone.dart';
import 'package:g6618_gui/widgets/glow_value.dart';
import 'package:g6618_gui/widgets/gui_button.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';
import 'package:g6618_gui/widgets/trend_chart.dart';

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
      expect(
        find.descendant(
          of: find.byType(FaultComposition),
          matching: find.textContaining('U 相'),
        ),
        findsOneWidget,
      );
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
      expect(
        find.descendant(
          of: find.byType(FaultComposition),
          matching: find.textContaining('V 相'),
        ),
        findsOneWidget,
      );
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

  group('热与负载移驻曲线区右列', () {
    testWidgets('饼图已移除：曲线区不再出现相电流占比面板', (tester) async {
      final state = AppState.test(
        status: makeStatus(iU: 1, iV: 2, iW: 1),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(find.byKey(const Key('phase_share_panel')), findsNothing);
      expect(find.text('相电流占比'), findsNothing);
    });

    testWidgets('六条 HeatLoadBar 迁入 scope_heat_panel', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      final panel = find.byKey(const Key('scope_heat_panel'));
      expect(panel, findsOneWidget);
      expect(
        find.descendant(of: panel, matching: find.text('热与负载 · 相对门限')),
        findsOneWidget,
      );
      expect(find.byType(HeatLoadBar), findsNWidgets(6));
    });

    testWidgets('无门限：六行读数一律 —，不冒充 0 %', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      final panel = find.byKey(const Key('scope_heat_panel'));
      expect(
        find.descendant(of: panel, matching: find.text('—')),
        findsNWidgets(6),
      );
      // No fabricated `0 %`: the mould row and every unloaded threshold stay —.
      expect(
        find.descendant(of: panel, matching: find.text('0')),
        findsNothing,
      );
    });

    testWidgets('门限载入：读数按比例取三色语义', (tester) async {
      final state = AppState.test(status: makeStatus(iU: 30, iV: 70, iW: 90));
      addTearDown(state.dispose);
      state.values['motor.i_peak_10s_a'] = ParamValueDto(ty: 0, raw: f32(100));

      await _pump(tester, ScopeZone(state: state));

      final panel = find.byKey(const Key('scope_heat_panel'));
      expect(
        find.descendant(of: panel, matching: find.text('30')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: panel, matching: find.text('70')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: panel, matching: find.text('90')),
        findsOneWidget,
      );
    });
  });

  group('曲线区 1kHz 趋势图', () {
    testWidgets('ScopeZone 渲染 TrendChart，图注含 1 kHz 采样与 STREAM_SUB 预留', (
      tester,
    ) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(find.byType(TrendChart), findsOneWidget);
      expect(find.textContaining('1 kHz 采样'), findsOneWidget);
      expect(find.textContaining('STREAM_SUB'), findsWidgets);
    });

    testWidgets('statusHistory 注入后趋势图绘制三通道数据', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        history: <StatusDto>[makeStatus(iU: 1), makeStatus(iU: 2)],
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      final chart = tester.widget<TrendChart>(find.byType(TrendChart));
      expect(chart.history, hasLength(2));
      expect(chart.history.last.iU, 2);
    });
  });

  group('faultBitLabel', () {
    test('已知位返回中文语义，未知位返回 hex 回退', () {
      expect(faultBitLabel(0), '过流 L1 · U 相');
      expect(faultBitLabel(6), '母线过压');
      expect(faultBitLabel(20), 'bit 20 (0x00100000)');
    });
  });

  group('HeatLoadBar 三色门限（v3 tone mapping）', () {
    test('比例三带：<50% green、50–85% amber、>85% red', () {
      expect(HeatLoadBar.toneForRatio(null), Tone.neutral);
      expect(HeatLoadBar.toneForRatio(0.49), Tone.green);
      expect(HeatLoadBar.toneForRatio(0.5), Tone.amber);
      expect(HeatLoadBar.toneForRatio(0.85), Tone.amber);
      expect(HeatLoadBar.toneForRatio(0.86), Tone.red);
      expect(HeatLoadBar.toneForRatio(1.0), Tone.red);
    });

    testWidgets('读数与填充随比例取色；无门限保持 —', (tester) async {
      await _pump(
        tester,
        Column(
          children: const <Widget>[
            HeatLoadBar(name: 'U 相', value: 30, threshold: 100),
            HeatLoadBar(name: 'V 相', value: 70, threshold: 100),
            HeatLoadBar(name: 'W 相', value: 90, threshold: 100),
            HeatLoadBar(name: '模组', value: null, threshold: null),
          ],
        ),
      );

      expect(
        tester.widget<Text>(find.text('30')).style?.color,
        const GuiPalette().valueGreen,
      );
      expect(
        tester.widget<Text>(find.text('70')).style?.color,
        const GuiPalette().valueAmber,
      );
      expect(
        tester.widget<Text>(find.text('90')).style?.color,
        const GuiPalette().valueRed,
      );
      expect(find.text('—'), findsOneWidget);
    });
  });

  group('v3 面板框（HairlineFrame main）', () {
    testWidgets('状态监视：组外框 + 6 瓦片内框双层嵌套（ENC 已迁出）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, StatusZone(state: state));

      expect(find.byKey(const Key('status_grid_frame')), findsOneWidget);
      expect(find.byKey(const Key('status_frame_VBUS')), findsOneWidget);
      expect(find.byKey(const Key('status_frame_F_ELEC')), findsOneWidget);
      // Two encoder tiles moved to EncoderZone: only six remain here.
      expect(find.byType(StatusTile), findsNWidgets(6));
      expect(find.byKey(const Key('status_frame_ENC_R')), findsNothing);
      expect(find.byKey(const Key('status_frame_ENC_O')), findsNothing);
      final tileFrame = tester.widget<HairlineFrame>(
        find.byKey(const Key('status_frame_VBUS')),
      );
      expect(tileFrame.variant, HairlineVariant.main);
    });

    testWidgets('曲线区 / 故障构成 / 控制条各自为亮青面板', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(find.byKey(const Key('scope_curve_frame')), findsOneWidget);
      expect(find.byKey(const Key('scope_heat_panel')), findsOneWidget);
      expect(find.byKey(const Key('fault_frame')), findsOneWidget);
      expect(find.byKey(const Key('control_frame')), findsOneWidget);
      expect(find.byType(HairlineFrame), findsWidgets);
    });
  });

  group('v3 页面语义色', () {
    testWidgets('非告警测量瓦片为扁平中性白（tone neutral）', (tester) async {
      await _pump(
        tester,
        const StatusTile(
          label: 'VBUS',
          zh: '母线电压',
          value: 24.0,
          unit: 'V',
          precision: 1,
        ),
      );

      final value = tester.widget<Text>(find.text('24.0'));
      expect(value.style?.color, const GuiPalette().fgStrong);
      expect(value.style?.shadows, anyOf(isNull, isEmpty));
    });

    testWidgets('故障构成条目标题与红色条目（Tone.red）', (tester) async {
      final state = AppState.test(
        status: makeStatus(faultFlags: 0x1),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(find.text('故障构成与指令'), findsOneWidget);
      final entry = tester.widget<Text>(find.text('过流 L1 · U 相'));
      expect(entry.style?.color, const GuiPalette().valueRed);
    });

    testWidgets('控制按钮为 GuiButton 三级（启动 primary/清除故障 danger）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(
        tester
            .widget<GuiButton>(find.widgetWithText(GuiButton, '启动'))
            .variant,
        GuiButtonVariant.primary,
      );
      expect(
        tester
            .widget<GuiButton>(find.widgetWithText(GuiButton, '清除故障'))
            .variant,
        GuiButtonVariant.danger,
      );
      final freq = tester.widget<Text>(find.text('0.00'));
      expect(freq.style?.color, const GuiPalette().valueAmber);
      // Setpoint readout is a GlowValue: unit renders separately, uppercased.
      expect(find.text('HZ'), findsOneWidget);
    });
  });
}
