import 'dart:typed_data';

import '../bridge/frb/api.dart' show StatusDto;

/// Fixed-capacity ring history of the three phase currents, timestamped.
///
/// Backs the ScopeZone trend chart: the 1 kHz status watch delivers ~20 ms
/// batches, so the ring retains [capacity] samples = 60 s at 1 kHz. Samples are
/// stored in three flat [Float32List] channels (i_u / i_v / i_w) plus a
/// [Float64List] of arrival timestamps in seconds, so the chart can slice a
/// time window and decimate it without allocating a `List<StatusDto>` per
/// frame. Oldest-first index 0 is always the oldest retained sample; once full
/// the oldest sample is overwritten in place.
class ChartHistory {
  /// Creates an empty history. [sampleRateHz] is the nominal spacing used to
  /// timestamp batched samples (the watch runs at 1 kHz).
  ChartHistory({this.sampleRateHz = 1000});

  /// Ring capacity: 60_000 samples = 60 s at 1 kHz.
  static const int capacity = 60000;

  /// Number of stored channels (i_u / i_v / i_w).
  static const int channelCount = 3;

  /// Nominal sample rate [Hz] used to space timestamps within a batch.
  final int sampleRateHz;

  final Float32List _iU = Float32List(capacity);
  final Float32List _iV = Float32List(capacity);
  final Float32List _iW = Float32List(capacity);
  final Float64List _times = Float64List(capacity);

  int _start = 0;
  int _len = 0;
  double _nextTime = 0;
  int _version = 0;

  /// Number of retained samples, oldest-first.
  int get len => _len;

  /// True when no sample has ever been pushed (or after [clear]).
  bool get isEmpty => _len == 0;

  /// Monotonic revision, bumped on every mutation. Lets a painter detect new
  /// data even though the [ChartHistory] identity is stable.
  int get version => _version;

  /// Timestamp [s] of the oldest retained sample, or 0 when empty.
  double get firstTime => _len == 0 ? 0 : timeAt(0);

  /// Timestamp [s] of the newest retained sample, or 0 when empty.
  double get lastTime => _len == 0 ? 0 : timeAt(_len - 1);

  int _ring(int index) => (_start + index) % capacity;

  /// Value of [channel] (0=i_u, 1=i_v, 2=i_w) at oldest-first [index].
  double channelAt(int channel, int index) {
    final slot = _ring(index);
    return switch (channel) {
      0 => _iU[slot],
      1 => _iV[slot],
      2 => _iW[slot],
      _ => throw RangeError.index(channel, const <int>[0, 1, 2], 'channel'),
    };
  }

  /// Timestamp [s] of the oldest-first sample at [index].
  double timeAt(int index) => _times[_ring(index)];

  /// Appends every sample of [samples] in order, dropping the oldest once the
  /// ring is full. Timestamps advance by `1 / sampleRateHz` per sample.
  void pushBatch(List<StatusDto> samples) {
    if (samples.isEmpty) return;
    final period = 1.0 / sampleRateHz;
    for (final sample in samples) {
      final slot = (_start + _len) % capacity;
      _iU[slot] = sample.iU;
      _iV[slot] = sample.iV;
      _iW[slot] = sample.iW;
      _times[slot] = _nextTime;
      _nextTime += period;
      if (_len == capacity) {
        _start = (_start + 1) % capacity;
      } else {
        _len++;
      }
    }
    _version++;
  }

  /// Empties the ring without reallocating.
  void clear() {
    _start = 0;
    _len = 0;
    _nextTime = 0;
    _version++;
  }

  /// Oldest-first index of the first sample with `time >= [seconds]`, in
  /// `[0, len]`. `len` means every retained sample is older than [seconds].
  int lowerBoundTime(double seconds) {
    var low = 0;
    var high = _len;
    while (low < high) {
      final mid = (low + high) >> 1;
      if (timeAt(mid) < seconds) {
        low = mid + 1;
      } else {
        high = mid;
      }
    }
    return low;
  }

  /// Number of retained samples whose timestamp falls inside
  /// `[startSec, endSec]` (both inclusive).
  int countInWindow(double startSec, double endSec) {
    if (_len == 0 || endSec < startSec) return 0;
    final low = lowerBoundTime(startSec);
    final high = lowerBoundTime(endSec);
    var count = high - low;
    if (high < _len && timeAt(high) <= endSec) count++;
    return count;
  }
}
