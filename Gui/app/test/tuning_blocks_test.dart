import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/tuning_blocks.dart';
import 'package:g6618_gui/widgets/gui_button.dart';

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

ParamMetaDto _f32Meta(String name) => ParamMetaDto(
  index: 0,
  name: name,
  ty: 0,
  apply: 0,
  unit: '',
  min: 0,
  max: 100,
);

Future<void> _pump(WidgetTester tester, CurrentLoopPanel panel) {
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
  group('电流环 PI 暂存-写入', () {
    const kp = 'software.control.current_loop.kp';

    testWidgets('无暂存时「写入」禁用', (tester) async {
      final state = AppState.test(status: _status(), metas: const []);
      addTearDown(state.dispose);

      await _pump(tester, CurrentLoopPanel(state: state));

      final write = tester.widget<GuiButton>(
        find.widgetWithText(GuiButton, '写入'),
      );
      expect(write.onPressed, isNull);
    });

    testWidgets('暂存字段显示旧→新对比，写入只下发电流环子集', (tester) async {
      const external = 'software.fault.vbus_uv_v';
      final state = AppState.test(
        status: _status(),
        metas: <ParamMetaDto>[_f32Meta(kp), _f32Meta(external)],
      );
      addTearDown(state.dispose);
      state.values[kp] = ParamValueDto(ty: 0, raw: f32(1.0));
      final writes = <String>[];
      var commits = 0;
      state.onParamWrite = (name, ty, raw) async => writes.add(name);
      state.onParamCommit = () async => commits++;
      state.stageParam(kp, 0, f32(3.0));
      state.stageParam(external, 0, f32(9.0));

      await _pump(tester, CurrentLoopPanel(state: state));

      expect(find.byKey(const Key('field_staged_$kp')), findsOneWidget);
      expect(find.text('→'), findsOneWidget);
      expect(find.byKey(const Key('current_loop_pending_badge')), findsOneWidget);

      await tester.tap(find.text('写入'));
      await tester.pumpAndSettle();

      expect(writes, <String>[kp]);
      expect(commits, 1);
      expect(state.staged.containsKey(kp), isFalse);
      expect(state.staged.containsKey(external), isTrue);
    });
  });
}
