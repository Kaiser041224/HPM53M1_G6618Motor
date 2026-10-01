import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/control_zone.dart';
import 'package:g6618_gui/ui/limit_zone.dart';
import 'package:g6618_gui/ui/scope_zone.dart';
import 'package:g6618_gui/ui/status_zone.dart';
import 'package:g6618_gui/widgets/glow_value.dart';
import 'package:g6618_gui/widgets/gui_button.dart';
import 'package:g6618_gui/widgets/hairline_frame.dart';
import 'package:g6618_gui/widgets/stable_digits.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';
import 'package:g6618_gui/widgets/trend_chart.dart';
import 'package:g6618_gui/widgets/vfd_bar_painter.dart';

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
    testWidgets('按量纲渲染精度：V 1 位 / A 3 位', (tester) async {
      final state = AppState.test(
        status: makeStatus(vbus: 24.0, iU: 0.0, mod: 100),
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
      // I_IN estimate: 1.5 · 1 · 0 = 0 A, rendered 3 decimals.
      expect(
        find.descendant(
          of: find.byKey(const Key('status_tile_I_IN')),
          matching: find.text('0.000'),
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

  group('ControlZone 控制条与故障构成', () {
    testWidgets('控制失败 code 4 保留联锁 SnackBar 文案', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);
      state.onCtrl = (op) async => throw const FrbLikeError(code: 4);

      await _pump(tester, ControlZone(state: state));
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

      await _pump(tester, ControlZone(state: state));

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

      await _pump(tester, ControlZone(state: state));

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

      await _pump(tester, ControlZone(state: state));

      expect(find.textContaining('bit 20'), findsOneWidget);
    });

    testWidgets('无故障时不出条目', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ControlZone(state: state));

      expect(find.textContaining('过流'), findsNothing);
      expect(find.textContaining('bit '), findsNothing);
    });
  });

  group('热与负载移驻门限区（LimitZone）', () {
    testWidgets('饼图已移除：门限区不再出现相电流占比面板', (tester) async {
      final state = AppState.test(
        status: makeStatus(iU: 1, iV: 2, iW: 1),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, LimitZone(state: state));

      expect(find.byKey(const Key('phase_share_panel')), findsNothing);
      expect(find.text('相电流占比'), findsNothing);
    });

    testWidgets('六条 HeatLoadBar 迁入 limit_heat_panel', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, LimitZone(state: state));

      final panel = find.byKey(const Key('limit_heat_panel'));
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

      await _pump(tester, LimitZone(state: state));

      final panel = find.byKey(const Key('limit_heat_panel'));
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

      await _pump(tester, LimitZone(state: state));

      final panel = find.byKey(const Key('limit_heat_panel'));
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

    testWidgets('六行条显式携带量纲（NTC→℃ / 相→A / VBUS→V / 模组空）', (
      tester,
    ) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, LimitZone(state: state));

      String unitOf(String name) => tester
          .widget<HeatLoadBar>(
            find.byWidgetPredicate((w) => w is HeatLoadBar && w.name == name),
          )
          .unit!;
      expect(unitOf('NTC'), '℃');
      expect(unitOf('U 相'), 'A');
      expect(unitOf('V 相'), 'A');
      expect(unitOf('W 相'), 'A');
      expect(unitOf('VBUS'), 'V');
      expect(unitOf('模组'), '');
    });

    testWidgets('F_ELEC 示数按量纲着色（Hz→purple），名称统一青色', (tester) async {
      final state = AppState.test(
        status: makeStatus(iU: 1.82, iV: 1.75, iW: 1.80, freq: 50.0),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, LimitZone(state: state));

      final panel = find.byKey(const Key('limit_freq_panel'));
      final name = tester.widget<Text>(
        find.descendant(of: panel, matching: find.text('F_ELEC')),
      );
      expect(name.style?.color, const GuiPalette().accent);
      expect(name.style?.shadows, isNotEmpty);
      final freq = tester.widget<Text>(
        find.descendant(of: panel, matching: find.text('50.00')),
      );
      expect(freq.style?.color, const GuiPalette().valuePurple);
    });
  });

  group('波形区 1kHz 趋势图', () {
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

    testWidgets('chartHistory 注入后趋势图携带三通道数据', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        history: <StatusDto>[makeStatus(iU: 1), makeStatus(iU: 2)],
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      final chart = tester.widget<TrendChart>(find.byType(TrendChart));
      expect(chart.history.len, 2);
      expect(chart.history.channelAt(0, 1), 2.0);
    });
  });

  group('faultBitLabel', () {
    test('已知位返回中文语义，未知位返回 hex 回退', () {
      expect(faultBitLabel(0), '过流 L1 · U 相');
      expect(faultBitLabel(6), '母线过压');
      expect(faultBitLabel(20), 'bit 20 (0x00100000)');
    });
  });

  group('HeatLoadBar 量纲专色（VFD 竖排发光管条换装）', () {
    test('比例三带（toneForRatio）保留兼容语义', () {
      expect(HeatLoadBar.toneForRatio(null), Tone.neutral);
      expect(HeatLoadBar.toneForRatio(0.49), Tone.green);
      expect(HeatLoadBar.toneForRatio(0.5), Tone.amber);
      expect(HeatLoadBar.toneForRatio(0.85), Tone.amber);
      expect(HeatLoadBar.toneForRatio(0.86), Tone.red);
      expect(HeatLoadBar.toneForRatio(1.0), Tone.red);
    });

    testWidgets('管色 / 读数 tint 由量纲派生：A→green / ℃→orange / V→cyan', (
      tester,
    ) async {
      await _pump(
        tester,
        const Column(
          children: <Widget>[
            HeatLoadBar(name: 'U 相', value: 30, threshold: 100, unit: 'A'),
            HeatLoadBar(name: 'NTC', value: 70, threshold: 100, unit: '℃'),
            HeatLoadBar(name: 'VBUS', value: 80, threshold: 100, unit: 'V'),
            HeatLoadBar(name: '模组', value: null, threshold: null),
          ],
        ),
      );

      VfdBar barOf(String name) =>
          tester.widget<VfdBar>(find.byKey(Key('heat_bar_$name')));
      expect(barOf('U 相').tint, const GuiPalette().valueGreen);
      expect(barOf('NTC').tint, const GuiPalette().valueOrange);
      expect(barOf('VBUS').tint, const GuiPalette().valueCyan);
      // The % readout keeps the same dimension tint as the tube bank.
      expect(
        tester.widget<Text>(find.text('30')).style?.color,
        const GuiPalette().valueGreen,
      );
      expect(
        tester.widget<Text>(find.text('70')).style?.color,
        const GuiPalette().valueOrange,
      );
      expect(find.text('—'), findsOneWidget);
    });

    testWidgets('未显式给 unit 时由行名派生量纲（limit_zone 六行）', (tester) async {
      await _pump(
        tester,
        const Column(
          children: <Widget>[
            HeatLoadBar(name: 'NTC', value: 70, threshold: 100),
            HeatLoadBar(name: 'U 相', value: 70, threshold: 100),
            HeatLoadBar(name: 'VBUS', value: 70, threshold: 100),
          ],
        ),
      );
      VfdBar barOf(String name) =>
          tester.widget<VfdBar>(find.byKey(Key('heat_bar_$name')));
      expect(barOf('NTC').tint, const GuiPalette().valueOrange);
      expect(barOf('U 相').tint, const GuiPalette().valueGreen);
      expect(barOf('VBUS').tint, const GuiPalette().valueCyan);
    });
  });

  group('响度条峰值保持与门限刻度（VfdBar）', () {
    testWidgets('峰值线随当前值即时上移', (tester) async {
      await _pump(
        tester,
        const HeatLoadBar(name: 'U 相', value: 90, threshold: 100),
      );

      final bar = tester.widget<VfdBar>(find.byKey(const Key('heat_bar_U 相')));
      expect(bar.peak, closeTo(0.9, 1e-6));
    });

    testWidgets('峰值保持 800ms 后缓回落，且不低于当前值', (tester) async {
      await _pump(
        tester,
        const HeatLoadBar(name: 'U 相', value: 90, threshold: 100),
      );
      await _pump(
        tester,
        const HeatLoadBar(name: 'U 相', value: 10, threshold: 100),
      );

      // 800 ms hold: the marker has not moved yet.
      await tester.pump(const Duration(milliseconds: 800));
      expect(
        tester.widget<VfdBar>(find.byKey(const Key('heat_bar_U 相'))).peak,
        closeTo(0.9, 1e-6),
      );

      // 1 s of peak decay, sampled at the meter's 5 Hz cadence.
      for (var i = 0; i < 5; i++) {
        await tester.pump(const Duration(milliseconds: 200));
      }
      final peak = tester
          .widget<VfdBar>(find.byKey(const Key('heat_bar_U 相')))
          .peak!;
      expect(peak, lessThan(0.9));
      expect(peak, greaterThanOrEqualTo(0.1));
    });

    testWidgets('50% / 85% 门限刻度由 VfdBar 开启，刻度位置恒定', (tester) async {
      await _pump(
        tester,
        const HeatLoadBar(name: 'U 相', value: 90, threshold: 100),
      );

      expect(
        tester.widget<VfdBar>(find.byKey(const Key('heat_bar_U 相'))).warnBand,
        isTrue,
      );
      expect(VfdBarPainter.bandMarks, <double>[0.5, 0.85]);
    });

    testWidgets('无门限为空槽 + —，不画峰值线', (tester) async {
      await _pump(
        tester,
        const HeatLoadBar(name: '模组', value: null, threshold: null),
      );

      expect(find.text('—'), findsOneWidget);
      final bar = tester.widget<VfdBar>(find.byKey(const Key('heat_bar_模组')));
      expect(bar.peak, isNull);
      expect(bar.ratio, 0.0);
    });
  });

  group('门限条越限语义（>100%）', () {
    const palette = GuiPalette();

    testWidgets('>100%：百分比显示真实越限值（105%）', (tester) async {
      await _pump(
        tester,
        const HeatLoadBar(name: 'U 相', value: 105, threshold: 100, unit: 'A'),
      );

      expect(find.text('105'), findsOneWidget);
      expect(find.text('%'), findsOneWidget);
      // The ratio is preserved (not clamped) so the label is honest.
      expect(
        tester
            .widget<VfdBar>(find.byKey(const Key('heat_bar_U 相')))
            .ratio,
        closeTo(1.05, 1e-6),
      );
    });

    testWidgets('>100%：柱体与百分比文字转警告橙', (tester) async {
      await _pump(
        tester,
        const HeatLoadBar(name: 'VBUS', value: 120, threshold: 100, unit: 'V'),
      );

      expect(
        tester.widget<VfdBar>(find.byKey(const Key('heat_bar_VBUS'))).tint,
        palette.valueOrange,
      );
      expect(
        tester.widget<Text>(find.text('120')).style?.color,
        palette.valueOrange,
      );
    });

    testWidgets('百分比封顶 999，防越限溢出', (tester) async {
      await _pump(
        tester,
        const HeatLoadBar(name: 'U 相', value: 2000, threshold: 100, unit: 'A'),
      );

      expect(find.text('999'), findsOneWidget);
      expect(find.text('%'), findsOneWidget);
    });

    testWidgets('≤100%：量纲专色保持（V→cyan / ℃→orange）', (tester) async {
      await _pump(
        tester,
        const Column(
          children: <Widget>[
            HeatLoadBar(name: 'VBUS', value: 80, threshold: 100, unit: 'V'),
            HeatLoadBar(name: 'NTC', value: 95, threshold: 100, unit: '℃'),
          ],
        ),
      );

      expect(
        tester.widget<VfdBar>(find.byKey(const Key('heat_bar_VBUS'))).tint,
        palette.valueCyan,
      );
      expect(
        tester.widget<VfdBar>(find.byKey(const Key('heat_bar_NTC'))).tint,
        palette.valueOrange,
      );
      // NTC's orange here is the dimension colour, not the over-limit switch.
      expect(HeatLoadBar.overLimitTint(palette, 0.95), isNull);
      expect(HeatLoadBar.overLimitTint(palette, 1.05), palette.valueOrange);
    });
  });

  group('v3 面板框（HairlineFrame main）', () {
    testWidgets('状态监视：组外框 + 5 瓦片内框双层嵌套（ENC 已迁出）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, StatusZone(state: state));

      expect(find.byKey(const Key('status_grid_frame')), findsOneWidget);
      expect(find.byKey(const Key('status_frame_VBUS')), findsOneWidget);
      expect(find.byKey(const Key('status_frame_NTC_STATOR')), findsOneWidget);
      // Two encoder tiles moved to EncoderZone: only five remain here.
      expect(find.byType(StatusTile), findsNWidgets(5));
      expect(find.byKey(const Key('status_frame_ENC_R')), findsNothing);
      expect(find.byKey(const Key('status_frame_ENC_O')), findsNothing);
      final tileFrame = tester.widget<HairlineFrame>(
        find.byKey(const Key('status_frame_VBUS')),
      );
      expect(tileFrame.variant, HairlineVariant.main);
    });

    testWidgets('波形面板为亮青框', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ScopeZone(state: state));

      expect(find.byKey(const Key('scope_curve_frame')), findsOneWidget);
      expect(find.byType(HairlineFrame), findsWidgets);
    });

    testWidgets('控制 / 故障 / 门限各自为亮青面板', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ControlZone(state: state));
      expect(find.byKey(const Key('fault_frame')), findsOneWidget);
      expect(find.byKey(const Key('control_frame')), findsOneWidget);
      expect(find.byType(HairlineFrame), findsWidgets);

      await _pump(tester, LimitZone(state: state));
      expect(find.byKey(const Key('limit_heat_panel')), findsOneWidget);
      expect(find.byKey(const Key('limit_freq_panel')), findsOneWidget);
      expect(find.byType(HairlineFrame), findsWidgets);
    });
  });

  group('v5 页面语义色', () {
    testWidgets('非告警测量瓦片按量纲着色：V→cyan（不再是中性白）', (tester) async {
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
      expect(value.style?.color, const GuiPalette().valueCyan);
      expect(value.style?.shadows, anyOf(isNull, isEmpty));
    });

    testWidgets('数值量纲专色：A→green / W→magenta / ℃→orange', (tester) async {
      await _pump(
        tester,
        const Column(
          children: <Widget>[
            StatusTile(
              label: 'I',
              zh: '电流',
              value: 1.0,
              unit: 'A',
              precision: 3,
            ),
            StatusTile(
              label: 'P',
              zh: '功率',
              value: 2.0,
              unit: 'W',
              precision: 1,
            ),
            StatusTile(
              label: 'NTC',
              zh: '温度',
              value: 3.0,
              unit: '℃',
              precision: 1,
            ),
          ],
        ),
      );

      expect(
        tester.widget<Text>(find.text('1.000')).style?.color,
        const GuiPalette().valueGreen,
      );
      expect(
        tester.widget<Text>(find.text('2.0')).style?.color,
        const GuiPalette().valueMagenta,
      );
      expect(
        tester.widget<Text>(find.text('3.0')).style?.color,
        const GuiPalette().valueOrange,
      );
    });

    testWidgets('参数名 / 瓦片标签统一青色 + 弱青辉光', (tester) async {
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

      final label = tester.widget<Text>(find.text('VBUS'));
      expect(label.style?.color, const GuiPalette().accent);
      expect(label.style?.shadows, isNotEmpty);
    });

    testWidgets('EST 反推值的注记带紫标识（valuePurple）', (tester) async {
      await _pump(
        tester,
        const StatusTile(
          label: 'P_IN',
          zh: '输入功率',
          value: 595.2,
          unit: 'W',
          precision: 1,
          note: 'EST · 相电流反推',
        ),
      );

      final note = tester.widget<Text>(find.text('EST · 相电流反推'));
      expect(note.style?.color, const GuiPalette().valuePurple);
    });

    testWidgets('故障构成条目标题与红色条目（Tone.red）', (tester) async {
      final state = AppState.test(
        status: makeStatus(faultFlags: 0x1),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, ControlZone(state: state));

      expect(find.text('故障构成与指令'), findsOneWidget);
      final entry = tester.widget<Text>(find.text('过流 L1 · U 相'));
      expect(entry.style?.color, const GuiPalette().valueRed);
    });

    testWidgets('控制按钮为 GuiButton 三级（启动 primary/清除故障 danger）', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ControlZone(state: state));

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
      // The setpoint readout lives in the control frame; scope it so the
      // assertion is stable even if other panels render an F_ELEC value.
      final control = find.byKey(const Key('control_frame'));
      // v5 量纲专色：FREQ（Hz）→ 紫。
      final freq = tester.widget<Text>(
        find.descendant(of: control, matching: find.text('0.00')),
      );
      expect(freq.style?.color, const GuiPalette().valuePurple);
      // Setpoint readout is a GlowValue: unit renders separately, uppercased.
      expect(
        find.descendant(of: control, matching: find.text('HZ')),
        findsOneWidget,
      );
    });
  });

  group('瓦片数值 5 Hz 限频（StableDigits）', () {
    testWidgets('瓦片数值子树包含 StableDigits', (tester) async {
      final state = AppState.test(
        status: makeStatus(vbus: 24.0),
        metas: const [],
      );
      addTearDown(state.dispose);

      await _pump(tester, StatusZone(state: state));

      expect(
        find.descendant(
          of: find.byKey(const Key('status_tile_VBUS')),
          matching: find.byType(StableDigits),
        ),
        findsOneWidget,
      );
    });

    testWidgets('新值 <200ms 保持旧字，≥200ms 才换字', (tester) async {
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
      expect(find.text('24.0'), findsOneWidget);

      await _pump(
        tester,
        const StatusTile(
          label: 'VBUS',
          zh: '母线电压',
          value: 25.0,
          unit: 'V',
          precision: 1,
        ),
      );
      await tester.pump(const Duration(milliseconds: 199));
      expect(find.text('25.0'), findsNothing);
      expect(find.text('24.0'), findsOneWidget);

      await tester.pump(const Duration(milliseconds: 1));
      expect(find.text('25.0'), findsOneWidget);
      expect(find.text('24.0'), findsNothing);
    });

    testWidgets('三态语义不变：stale 仍显示 — 单位，不被限频掩盖', (tester) async {
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
    });
  });
}
