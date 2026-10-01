import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/pid_zone.dart';

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
  });
}
