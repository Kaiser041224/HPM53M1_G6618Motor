import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/pid_zone.dart';
import 'package:g6618_gui/widgets/gui_button.dart';

StatusDto makeStatus() => StatusDto(
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
  encRotorDeg: 0,
  encOutputDeg: 0,
);

int f32(double v) =>
    (ByteData(4)..setFloat32(0, v, Endian.little)).getInt32(0, Endian.little);

Future<void> _pump(WidgetTester tester, PidZone zone) {
  tester.view.physicalSize = const Size(1600, 1600);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(body: SingleChildScrollView(child: zone)),
    ),
  );
}

ParamMetaDto _f32Meta(String name, {double min = 0, double max = 100}) =>
    ParamMetaDto(
      index: 0,
      name: name,
      ty: 0,
      apply: 0,
      unit: '',
      min: min,
      max: max,
    );

void main() {
  group('速度环 / 角度环 PID 调整入口', () {
    testWidgets('速度环 / 角度环两组标题与 4 个参数位渲染', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, PidZone(state: state));

      expect(find.byKey(const Key('speed_loop_frame')), findsOneWidget);
      expect(find.byKey(const Key('angle_loop_frame')), findsOneWidget);
      expect(find.text('速度环'), findsOneWidget);
      expect(find.text('角度环'), findsOneWidget);
      expect(find.text('比例增益 kp'), findsNWidgets(2));
      expect(find.text('积分增益 ki'), findsNWidgets(2));
    });

    testWidgets('缺参时两组共 4 个参数如实显示「设备未提供」', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, PidZone(state: state));

      expect(find.text('设备未提供'), findsNWidgets(4));
    });

    testWidgets('设备提供元数据时按全限定名渲染当前值', (tester) async {
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[_f32Meta('software.control.speed_loop.kp')],
      );
      addTearDown(state.dispose);
      state.values['software.control.speed_loop.kp'] = ParamValueDto(
        ty: 0,
        raw: f32(1.5),
      );

      await _pump(tester, PidZone(state: state));

      // speed_loop.kp 有值 → 1.50；其余缺值仍「设备未提供」（4 - 1 = 3）。
      expect(find.text('1.50'), findsOneWidget);
      expect(find.text('设备未提供'), findsNWidgets(3));
    });

    testWidgets('暂存字段显示「旧值 → 新值」对比：旧值划线，新值 stPending', (tester) async {
      const kp = 'software.control.speed_loop.kp';
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[_f32Meta(kp, min: 0, max: 10)],
      );
      addTearDown(state.dispose);
      state.values[kp] = ParamValueDto(ty: 0, raw: f32(1.5));
      state.stageParam(kp, 0, f32(2.0));

      await _pump(tester, PidZone(state: state));

      final staged = tester.widget<Text>(
        find.byKey(const Key('field_staged_$kp')),
      );
      expect(staged.data, '2.00');
      expect(staged.style?.color, const GuiPalette().stPending);

      final strike = tester.widget<Container>(
        find.byKey(const Key('field_strike_$kp')),
      );
      expect(strike.color, const GuiPalette().fgDim);
      expect(find.text('→'), findsOneWidget);
    });

    testWidgets('无暂存时 PidZone「写入」禁用', (tester) async {
      final state = AppState.test(status: makeStatus(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, PidZone(state: state));

      final write = tester.widget<GuiButton>(
        find.widgetWithText(GuiButton, '写入'),
      );
      expect(write.onPressed, isNull);
    });

    testWidgets('有暂存时「写入」可用，且只下发速度环+角度环名集合', (tester) async {
      const kp = 'software.control.speed_loop.kp';
      const external = 'software.fault.vbus_ov_v';
      final state = AppState.test(
        status: makeStatus(),
        metas: <ParamMetaDto>[
          _f32Meta(kp, min: 0, max: 10),
          ParamMetaDto(
            index: 1,
            name: external,
            ty: 0,
            apply: 0,
            unit: 'V',
            min: 0,
            max: 80,
          ),
        ],
      );
      addTearDown(state.dispose);
      final writes = <String>[];
      var commits = 0;
      state.onParamWrite = (name, ty, raw) async => writes.add(name);
      state.onParamCommit = () async => commits++;
      state.stageParam(kp, 0, f32(2.0));
      state.stageParam(external, 0, 123);

      await _pump(tester, PidZone(state: state));

      expect(find.byKey(const Key('pid_pending_badge')), findsOneWidget);
      final write = tester.widget<GuiButton>(
        find.widgetWithText(GuiButton, '写入'),
      );
      expect(write.onPressed, isNotNull);

      await tester.tap(find.text('写入'));
      await tester.pumpAndSettle();

      expect(writes, <String>[kp]);
      expect(commits, 1);
      expect(state.staged.containsKey(kp), isFalse);
      expect(state.staged[external], 123);
    });
  });
}
