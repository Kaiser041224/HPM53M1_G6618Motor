import 'package:flutter_test/flutter_test.dart';
import 'package:g6618_gui/bridge/frb/api.dart';
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';

/// Minimal status snapshot for freshness-seeding tests.
StatusDto makeStatus({int faultFlags = 0}) => StatusDto(
  faultFlags: faultFlags,
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

  test('connect() 成功后从 PROTO_INFO 读取型号/协议号并建立会话计时', () async {
    final state = AppState.test();
    addTearDown(state.dispose);
    state.onConnect = (addr) async => (1, 0, 0x03, 'G6618 Motor Controller');

    final ok = await state.connect('127.0.0.1:9000');

    expect(ok, isTrue);
    expect(state.connected, isTrue);
    expect(state.deviceName, 'G6618 Motor Controller');
    expect(state.protoMajor, 1);
    expect(state.protoMinor, 0);
    expect(state.capabilities, 0x03);
    expect(state.sessionUptime, isNotNull);
    expect(state.statusTimer, isNotNull);
  });

  test('disconnect() 清空会话时长与握手段，使顶栏诚实回落为 —', () async {
    final state = AppState.test();
    addTearDown(state.dispose);
    state.onConnect = (addr) async => (1, 0, 0x03, 'G6618 Motor Controller');
    await state.connect('127.0.0.1:9000');

    await state.disconnect();

    expect(state.connected, isFalse);
    expect(state.deviceName, isEmpty);
    expect(state.sessionUptime, isNull);
    expect(state.attempts, 0);
    expect(state.statusTimer, isNull);
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

  group('数据三态 freshnessOf', () {
    test('从未更新 → unknown；刚更新 → live', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      expect(state.freshnessOf('status_tile_VBUS'), Freshness.unknown);

      state.seedFreshness('status_tile_VBUS', DateTime.now());

      expect(state.freshnessOf('status_tile_VBUS'), Freshness.live);
    });

    test('超过 1s（2 个 500ms 轮询周期）未更新 → stale', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      state.seedFreshness(
        'status_tile_I_V',
        DateTime.now().subtract(const Duration(seconds: 2)),
      );

      expect(state.freshnessOf('status_tile_I_V'), Freshness.stale);
    });

    test('阈值边界：900ms 仍 live，1100ms 转 stale', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      state.seedFreshness(
        'rtt',
        DateTime.now().subtract(const Duration(milliseconds: 900)),
      );
      expect(state.freshnessOf('rtt'), Freshness.live);

      state.seedFreshness(
        'rtt',
        DateTime.now().subtract(const Duration(milliseconds: 1100)),
      );
      expect(state.freshnessOf('rtt'), Freshness.stale);
    });

    test('带 status 的测试构造令瓦片 live；无 status 则 unknown', () {
      final withStatus = AppState.test(status: makeStatus());
      addTearDown(withStatus.dispose);
      expect(withStatus.freshnessOf('status_tile_I_V'), Freshness.live);

      final bare = AppState.test();
      addTearDown(bare.dispose);
      expect(bare.freshnessOf('status_tile_I_V'), Freshness.unknown);
    });

    test('断连清空时间戳 → 全体 unknown', () async {
      final state = AppState.test(status: makeStatus());
      addTearDown(state.dispose);
      state.seedFreshness('rtt', DateTime.now());

      await state.disconnect();

      expect(state.freshnessOf('status_tile_I_V'), Freshness.unknown);
      expect(state.freshnessOf('rtt'), Freshness.unknown);
    });

    test('staleStatusTiles 只列出已失效的瓦片', () {
      final state = AppState.test(status: makeStatus());
      addTearDown(state.dispose);
      state.seedFreshness(
        'status_tile_I_V',
        DateTime.now().subtract(const Duration(seconds: 3)),
      );

      expect(state.staleStatusTiles, <String>['status_tile_I_V']);
    });
  });

  group('rtt 往返实测（EMA α=0.2）', () {
    test('首个样本直接采用，后续样本指数平滑', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      state.recordRtt(100);
      expect(state.rttMs, closeTo(100, 1e-6));

      state.recordRtt(200);
      expect(state.rttMs, closeTo(120, 1e-6));
    });

    test('断连清空 rttMs', () async {
      final state = AppState.test();
      addTearDown(state.dispose);
      state.recordRtt(50);

      await state.disconnect();

      expect(state.rttMs, isNull);
    });
  });
}
