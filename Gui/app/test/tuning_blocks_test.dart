import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/tuning_blocks.dart';

StatusDto _status() => StatusDto(
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

ParamMetaDto _f32Meta(String name, {String unit = '', double max = 100}) =>
    ParamMetaDto(
      index: 0,
      name: name,
      ty: 0,
      apply: 0,
      unit: unit,
      min: 0,
      max: max,
    );

Future<void> _pump(WidgetTester tester, ProtectionLimitsPanel panel) {
  tester.view.physicalSize = const Size(1200, 1200);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  return tester.pumpWidget(
    MaterialApp(
      theme: buildGuiTheme(),
      home: Scaffold(body: SingleChildScrollView(child: panel)),
    ),
  );
}

void main() {
  group('保护门限面板', () {
    testWidgets('一个外框 + 3 个安全参数位，各带 SAFETY 徽标', (tester) async {
      final state = AppState.test(
        status: _status(),
        metas: <ParamMetaDto>[
          for (final (name, _) in ProtectionLimitsPanel.fields)
            _f32Meta(name),
        ],
      );
      addTearDown(state.dispose);

      await _pump(tester, ProtectionLimitsPanel(state: state));

      expect(find.byKey(const Key('protection_limits_frame')), findsOneWidget);
      expect(find.text('保护门限'), findsOneWidget);
      expect(find.text('过流阈值'), findsOneWidget);
      expect(find.text('母线过压'), findsOneWidget);
      expect(find.text('母线欠压'), findsOneWidget);
      expect(find.text('SAFETY'), findsNWidgets(3));
    });

    testWidgets('缺参时 3 个参数如实显示「设备未提供」', (tester) async {
      final state = AppState.test(status: _status(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, ProtectionLimitsPanel(state: state));

      expect(find.text('设备未提供'), findsNWidgets(3));
    });

    testWidgets('设备提供元数据时按全限定名渲染当前值', (tester) async {
      const name = 'software.fault.oc_trip_a';
      final state = AppState.test(
        status: _status(),
        metas: <ParamMetaDto>[_f32Meta(name, unit: 'A', max: 80)],
      );
      addTearDown(state.dispose);
      state.values[name] = ParamValueDto(ty: 0, raw: f32(12.5));

      await _pump(tester, ProtectionLimitsPanel(state: state));

      expect(find.text('12.50 A'), findsOneWidget);
      expect(find.text('设备未提供'), findsNWidgets(2));
    });
  });
}
