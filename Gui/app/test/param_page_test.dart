import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:g6618_gui/bridge/frb/api.dart';
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/ui/param_page.dart';

ParamMetaDto makeMeta(
  int index,
  String name, {
  int ty = 0,
  int apply = 0,
  String unit = '',
  double min = 0,
  double max = 0,
}) => ParamMetaDto(
  index: index,
  name: name,
  ty: ty,
  apply: apply,
  unit: unit,
  min: min,
  max: max,
);

void main() {
  testWidgets('参数面板由元数据自动生成：域分组 + 搜索过滤', (tester) async {
    final state = AppState.test(
      metas: [
        makeMeta(0, 'motor.pole_pairs', ty: 1, unit: '', min: 1, max: 64),
        makeMeta(
          1,
          'software.fault.vbus_ov_v',
          ty: 0,
          unit: 'V',
          min: 0,
          max: 80,
        ),
      ],
    );
    await tester.pumpWidget(
      MaterialApp(home: Scaffold(body: ParamPage(state: state))),
    );
    expect(find.text('motor.pole_pairs'), findsOneWidget);
    // 搜索过滤
    await tester.enterText(find.byType(TextField).first, 'vbus');
    await tester.pump();
    expect(find.text('motor.pole_pairs'), findsNothing);
    expect(find.text('software.fault.vbus_ov_v'), findsOneWidget);
  });

  testWidgets('范围越界输入被拒绝并提示', (tester) async {
    final state = AppState.test(
      metas: [
        makeMeta(
          0,
          'software.fault.vbus_ov_v',
          ty: 0,
          unit: 'V',
          min: 0,
          max: 80,
        ),
      ],
    );
    await tester.pumpWidget(
      MaterialApp(home: Scaffold(body: ParamPage(state: state))),
    );
    await tester.tap(find.text('software.fault.vbus_ov_v'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField).last, '120');
    await tester.tap(find.text('写入'));
    await tester.pumpAndSettle();
    expect(find.textContaining('范围'), findsOneWidget);
  });
}
