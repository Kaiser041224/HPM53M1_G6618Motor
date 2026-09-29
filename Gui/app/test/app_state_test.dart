import 'package:flutter_test/flutter_test.dart';
import 'package:g6618_gui/bridge/frb/api.dart';
import 'package:g6618_gui/state/app_state.dart';

void main() {
  test('connect() 失败后不启动轮询定时器，requestFailures 计 1 次', () async {
    final state = AppState.test(
      connectError: const FrbError(code: 1, message: 'connection refused'),
    );
    addTearDown(state.dispose);

    final ok = await state.connect('127.0.0.1:9000');

    expect(ok, isFalse);
    expect(state.connected, isFalse);
    expect(state.statusTimer, isNull);
    expect(state.requestFailures, 1);
  });

  test('控制请求抛 FrbError(code:4) 时 requestFailures 恰 +1 且记录设备码', () async {
    final state = AppState.test();
    addTearDown(state.dispose);
    state.onCtrl = (op) async =>
        throw const FrbError(code: 4, message: 'interlock active');

    final ok = await state.ctrlStart();

    expect(ok, isFalse);
    expect(state.requestFailures, 1);
    expect(state.lastErrorCode, 4);
    expect(state.lastError, contains('interlock active'));
  });
}
