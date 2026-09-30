import 'package:flutter_test/flutter_test.dart';
import 'package:g6618_gui/bridge/frb/api.dart';
import 'package:g6618_gui/widgets/chart_history.dart';

/// Minimal status sample carrying the three phase currents.
StatusDto sample({double iU = 0, double iV = 0, double iW = 0}) => StatusDto(
  faultFlags: 0,
  motorRunning: false,
  motorState: 0,
  freqHz: 0,
  modPct: 0,
  iU: iU,
  iV: iV,
  iW: iW,
  vbus: 0,
  ntcTemp: 0,
  encRotorDeg: 0,
  encOutputDeg: 0,
);

void main() {
  group('ChartHistory 环形历史', () {
    test('容量 60k；溢出丢弃最旧，最新保留', () {
      final history = ChartHistory();
      expect(ChartHistory.capacity, 60000);

      history.pushBatch(
        List<StatusDto>.generate(60005, (i) => sample(iU: i.toDouble())),
      );

      expect(history.len, ChartHistory.capacity);
      // Oldest five samples (0..4) fell off; the ring now starts at 5.
      expect(history.channelAt(0, 0), 5.0);
      expect(history.channelAt(0, history.len - 1), 60004.0);
    });

    test('三通道各自独立存储', () {
      final history = ChartHistory();
      history.pushBatch(<StatusDto>[sample(iU: 1, iV: 2, iW: 3)]);

      expect(history.channelAt(0, 0), 1.0);
      expect(history.channelAt(1, 0), 2.0);
      expect(history.channelAt(2, 0), 3.0);
    });

    test('时间戳按 1kHz 单调推进（秒）', () {
      final history = ChartHistory();
      history.pushBatch(
        List<StatusDto>.generate(100, (i) => sample(iU: i.toDouble())),
      );

      expect(history.timeAt(0), closeTo(0.0, 1e-9));
      expect(history.timeAt(50), closeTo(0.05, 1e-9));
      expect(history.timeAt(99), closeTo(0.099, 1e-9));
      // Monotonic.
      for (var i = 1; i < history.len; i++) {
        expect(history.timeAt(i), greaterThan(history.timeAt(i - 1)));
      }
    });

    test('时间窗切片：lowerBoundTime / countInWindow 正确', () {
      final history = ChartHistory();
      history.pushBatch(
        List<StatusDto>.generate(100, (i) => sample(iU: i.toDouble())),
      );

      expect(history.lowerBoundTime(-1), 0);
      expect(history.lowerBoundTime(0.010), 10);
      expect(history.lowerBoundTime(0.0505), 51);
      expect(history.lowerBoundTime(10), history.len);

      // [10ms, 19.5ms] covers samples 10..19 inclusive = 10 samples.
      expect(history.countInWindow(0.010, 0.0195), 10);
      expect(history.countInWindow(0.2, 1.0), 0);
    });

    test('clear 清空', () {
      final history = ChartHistory();
      history.pushBatch(<StatusDto>[sample(iU: 1)]);
      history.clear();
      expect(history.len, 0);
      expect(history.isEmpty, isTrue);
    });
  });
}
