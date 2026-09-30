import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:g6618_gui/bridge/frb/api.dart';
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/widgets/chart_history.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';

/// Minimal status snapshot for freshness-seeding tests.
StatusDto makeStatus({int faultFlags = 0, double iU = 0}) => StatusDto(
  faultFlags: faultFlags,
  motorRunning: false,
  motorState: 0,
  freqHz: 0,
  modPct: 0,
  iU: iU,
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

  test('connect() 成功后自动触发 loadMetas（恰 1 次，不阻塞连接返回）', () async {
    final state = AppState.test();
    addTearDown(state.dispose);
    state.onConnect = (addr) async => (1, 0, 0x03, 'G6618 Motor Controller');
    var loads = 0;
    state.onLoadMetas = () => loads++;

    final ok = await state.connect('127.0.0.1:9000');

    expect(ok, isTrue);
    expect(loads, 1);
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

    test('超过 100ms（约 5 个 1kHz 批）未更新 → stale', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      state.seedFreshness(
        'status_tile_I_V',
        DateTime.now().subtract(const Duration(milliseconds: 200)),
      );

      expect(state.freshnessOf('status_tile_I_V'), Freshness.stale);
    });

    test('阈值边界：90ms 仍 live，110ms 转 stale', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      state.seedFreshness(
        'rtt',
        DateTime.now().subtract(const Duration(milliseconds: 90)),
      );
      expect(state.freshnessOf('rtt'), Freshness.live);

      state.seedFreshness(
        'rtt',
        DateTime.now().subtract(const Duration(milliseconds: 110)),
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

    test('连接态测试构造令 CRC/RESYNC/RETRY/帧 计数 live', () {
      final state = AppState.test(connected: true);
      addTearDown(state.dispose);

      for (final key in const <String>[
        AppState.keyCrc,
        AppState.keyResync,
        AppState.keyRetry,
        AppState.keyFrames,
      ]) {
        expect(state.freshnessOf(key), Freshness.live, reason: key);
      }
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

  group('chartHistory 60s 环形历史', () {
    test('recordHistory 追加样本，超过 60k 丢弃最旧', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      for (var i = 0; i < 60005; i++) {
        state.recordHistory(makeStatus(iU: i.toDouble()));
      }

      expect(state.chartHistory.len, ChartHistory.capacity);
      expect(ChartHistory.capacity, 60000);
      expect(state.chartHistory.channelAt(0, 0), 5.0);
      expect(state.chartHistory.channelAt(0, state.chartHistory.len - 1), 60004.0);
    });

    test('AppState.test 可注入 history（默认空）', () {
      final injected = AppState.test(
        history: <StatusDto>[makeStatus(iU: 1), makeStatus(iU: 2)],
      );
      addTearDown(injected.dispose);
      expect(injected.chartHistory.len, 2);
      expect(injected.chartHistory.channelAt(0, 1), 2.0);

      final bare = AppState.test();
      addTearDown(bare.dispose);
      expect(bare.chartHistory.isEmpty, isTrue);
    });

    test('disconnect 清空 chartHistory', () async {
      final state = AppState.test(history: <StatusDto>[makeStatus(iU: 1)]);
      addTearDown(state.dispose);

      await state.disconnect();

      expect(state.chartHistory.isEmpty, isTrue);
    });
  });

  group('1kHz 批注入', () {
    test('一批样本全部入环形，status 取批内最新', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      state.feedBatch(
        StatusBatchDto(
          samples: <StatusDto>[
            makeStatus(iU: 1),
            makeStatus(iU: 2),
            makeStatus(iU: 3),
          ],
          seq: BigInt.from(1),
        ),
      );

      expect(state.chartHistory.len, 3);
      expect(state.status!.iU, 3);
    });

    test('节流上限常量为 25 Hz（40 ms 通知窗口）', () {
      expect(kUiRefreshHz, 25);
    });

    testWidgets('UI 刷新节流：40ms 窗口内喂 10 批 → notify ≤ 1 次', (tester) async {
      final state = AppState.test();
      var notifies = 0;
      state.addListener(() => notifies++);

      for (var b = 0; b < 10; b++) {
        state.feedBatch(
          StatusBatchDto(
            samples: <StatusDto>[
              for (var i = 0; i < 16; i++) makeStatus(iU: i.toDouble()),
            ],
            seq: BigInt.from(b + 1),
          ),
        );
      }

      // Data within the batch is applied immediately ...
      expect(state.chartHistory.len, 160);
      // ... but notifications are coalesced onto the 25 Hz (40 ms) tick.
      await tester.pump(const Duration(milliseconds: 40));
      expect(notifies, lessThanOrEqualTo(1));

      // Dispose inside the body: the pending periodic timer would otherwise
      // trip the widget tester's end-of-test timer check.
      state.dispose();
    });

    testWidgets('批次通知最终送达（40ms 一次）', (tester) async {
      final state = AppState.test();
      var notifies = 0;
      state.addListener(() => notifies++);

      state.feedBatch(
        StatusBatchDto(
          samples: <StatusDto>[makeStatus(iU: 1)],
          seq: BigInt.one,
        ),
      );
      expect(notifies, 0);

      await tester.pump(const Duration(milliseconds: 41));
      expect(notifies, greaterThanOrEqualTo(1));

      state.dispose();
    });

    test('seq 非连续时计数丢批，连续则不计', () {
      final state = AppState.test();
      addTearDown(state.dispose);

      state.feedBatch(
        StatusBatchDto(samples: <StatusDto>[makeStatus()], seq: BigInt.from(1)),
      );
      state.feedBatch(
        StatusBatchDto(samples: <StatusDto>[makeStatus()], seq: BigInt.from(2)),
      );
      expect(state.droppedBatches, 0);

      // Jump 2 -> 6: batches 3, 4, 5 never arrived.
      state.feedBatch(
        StatusBatchDto(samples: <StatusDto>[makeStatus()], seq: BigInt.from(6)),
      );
      expect(state.droppedBatches, 3);
    });

    test('注入 batch 流后 connect 订阅，批到达即刷新', () async {
      final controller = StreamController<StatusBatchDto>();
      addTearDown(controller.close);
      final state = AppState.test(batch: controller.stream);
      addTearDown(state.dispose);
      state.onConnect = (addr) async => (1, 0, 0x03, 'G6618 Motor Controller');

      await state.connect('127.0.0.1:9000');
      expect(state.connected, isTrue);

      controller.add(
        StatusBatchDto(samples: <StatusDto>[makeStatus(iU: 7)], seq: BigInt.one),
      );
      await Future<void>.delayed(Duration.zero);

      expect(state.status!.iU, 7);
      expect(state.freshnessOf(AppState.statusTileKey('I_U')), Freshness.live);

      await state.disconnect();
      expect(state.chartHistory.isEmpty, isTrue);
      expect(state.droppedBatches, 0);
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
