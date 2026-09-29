import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:g6618_gui/bridge/frb/api.dart';
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/ui/dashboard_page.dart';

/// Bridge-like failure carrying a device error [code].
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

void main() {
  testWidgets('故障锁存时 START 显示联锁提示', (tester) async {
    final state = AppState.test(status: makeStatus(faultFlags: 0x1), metas: []);
    state.onCtrl = (op) async => throw const FrbLikeError(code: 4);
    await tester.pumpWidget(
      MaterialApp(home: Scaffold(body: DashboardPage(state: state))),
    );
    await tester.tap(find.text('启动'));
    await tester.pumpAndSettle();
    final snack = find.byType(SnackBar);
    expect(
      find.descendant(of: snack, matching: find.textContaining('联锁')),
      findsOneWidget,
    );
    expect(
      find.descendant(of: snack, matching: find.textContaining('FAULT_CLEAR')),
      findsOneWidget,
    );
  });

  testWidgets('状态卡显示母线电压与转速', (tester) async {
    final state = AppState.test(
      status: makeStatus(vbus: 48.2, freq: 100),
      metas: [],
    );
    await tester.pumpWidget(
      MaterialApp(home: Scaffold(body: DashboardPage(state: state))),
    );
    expect(find.textContaining('48.2'), findsOneWidget);
    expect(find.textContaining('100.0'), findsOneWidget);
  });
}
