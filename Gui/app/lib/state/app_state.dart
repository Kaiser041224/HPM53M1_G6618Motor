import 'dart:async';

import 'package:flutter/foundation.dart';

import '../bridge/frb/api.dart' as frb;
import '../widgets/stale_gate.dart' show Freshness;

// Re-exported so the UI layer depends only on AppState (ui -> state -> bridge)
// and never imports the generated bridge code directly.
export '../bridge/frb/api.dart'
    show ParamMetaDto, ParamValueDto, StatusBatchDto, StatusDto, StatsDto;

/// App-wide observable state.
///
/// Holds the connection state plus the device-side statistics that the
/// persistent status bar shows. Parameter table, status snapshot and control
/// state are added by later tasks.
class AppState extends ChangeNotifier {
  AppState();

  bool connected = false;
  String lastError = '';

  /// Device-side code of [lastError], or -1 when unavailable.
  int lastErrorCode = -1;

  String deviceName = '';

  /// PROTO_INFO handshake fields: protocol version and capability bitmask.
  int protoMajor = 0;
  int protoMinor = 0;
  int capabilities = 0;

  /// Wall-clock start of the current connection session; null when
  /// disconnected.
  DateTime? sessionStart;

  /// Elapsed time since [sessionStart], refreshed by the 500 ms stats poll.
  /// Null while disconnected so the top bar can honestly show `—`.
  Duration? sessionUptime;

  /// Round-trip time of the last `statsGet`, in milliseconds. Measured around
  /// the bridge call and smoothed with an EMA (α = [rttEmaAlpha]); null while
  /// disconnected so the UI honestly shows `—`.
  double? rttMs;

  /// EMA smoothing factor for [rttMs]: each sample moves the estimate 20%.
  static const double rttEmaAlpha = 0.2;

  /// A live value that has not been refreshed for longer than this degrades to
  /// [Freshness.stale]. With a 1 kHz batched feed (≈20 ms per batch) this is
  /// five missed batches — tight enough that a stalled watch shows up promptly
  /// (design-system §6 rule 2).
  static const Duration kStaleAfter = Duration(milliseconds: 100);

  /// Freshness key of a status tile; matches the `status_zone.dart` widget keys
  /// (`status_tile_<tile>`).
  static String statusTileKey(String tile) => 'status_tile_$tile';

  /// Freshness key of the measured STATUS_GET round-trip time.
  static const String keyRtt = 'rtt';

  /// Freshness key of the device CRC-error counter (`stats.crcErrors`).
  static const String keyCrc = 'crc';

  /// Freshness key of the device reconnect counter (`stats.reconnects`).
  static const String keyResync = 'resync';

  /// Freshness key of the failed-request counter.
  static const String keyRetry = 'retry';

  /// Freshness key of the client-side frame attempt counter (`stats.attempts`).
  static const String keyFrames = 'frames';

  /// Every status tile freshness key, in `status_zone.dart` order.
  static const List<String> statusTileKeys = <String>[
    'status_tile_VBUS',
    'status_tile_I_U',
    'status_tile_I_V',
    'status_tile_I_W',
    'status_tile_NTC',
    'status_tile_ENC_R',
    'status_tile_ENC_O',
    'status_tile_F_ELEC',
  ];

  /// Last-updated wall clock per [freshnessOf] key. Absent = never read.
  final Map<String, DateTime> _freshAt = {};

  /// Device-side counters, refreshed by the 500 ms status poll.
  int reconnects = 0;
  int crcErrors = 0;

  /// Client-side frame attempt counter (`stats.attempts`).
  int attempts = 0;

  /// Client-side count of failed polls / requests.
  int requestFailures = 0;

  /// Metadata for every device parameter, loaded via [loadMetas].
  List<frb.ParamMetaDto> metas = [];

  /// Last known value of each parameter, keyed by name.
  final Map<String, frb.ParamValueDto> values = {};

  /// Names written locally but not yet flushed to flash via [commit].
  final Set<String> dirty = {};

  /// True while [loadMetas] is in flight.
  bool loadingParams = false;

  /// Latest device status sample; the newest sample of the last batch, so a
  /// tile read here is current to within one ~20 ms batch. Cleared on
  /// [disconnect].
  frb.StatusDto? status;

  /// Oldest-first ring buffer of the last [statusHistoryLimit] status samples,
  /// appended once per batch by the 1 kHz watch (≈20 ms batches). Drives the
  /// ScopeZone trend chart — a 1 kHz, 2 s window preview, not the reserved
  /// `STREAM_SUB` stream. Cleared on [disconnect].
  final List<frb.StatusDto> statusHistory = [];

  /// Maximum retained samples: 2000 = 2 s at 1 kHz, matching
  /// `TrendChart.capacity`.
  static const int statusHistoryLimit = 2000;

  /// Appends [snapshot] to [statusHistory], dropping the oldest sample once
  /// the buffer exceeds [statusHistoryLimit].
  @visibleForTesting
  void recordHistory(frb.StatusDto snapshot) {
    statusHistory.add(snapshot);
    if (statusHistory.length > statusHistoryLimit) {
      statusHistory.removeRange(0, statusHistory.length - statusHistoryLimit);
    }
  }

  /// Batch seq of the last accepted frame, used for gap detection.
  BigInt? _lastSeq;

  /// Number of batches the device seq counter shows as missing (a gap in the
  /// monotonic `seq`). Reset per connection.
  int droppedBatches = 0;

  /// Active subscription to the 1 kHz status watch; null while disconnected or
  /// when the bridge does not provide the watch.
  StreamSubscription<frb.StatusBatchDto>? _batchSub;

  /// Sample rate requested from `start_status_watch` [Hz].
  static const int watchRateHz = 1000;

  /// Test-only replacement for the native watch stream. When set, [connect]
  /// subscribes to it instead of the bridge, so tests can push
  /// [frb.StatusBatchDto] frames and drive history / freshness.
  @visibleForTesting
  Stream<frb.StatusBatchDto>? batch;

  /// Applies one device batch: appends every sample to the ring, takes the
  /// batch's newest sample as [status], counts a seq gap and emits a **single**
  /// [notifyListeners] for the whole batch (≈50 Hz UI refresh — never one
  /// notification per sample).
  void _applyBatch(frb.StatusBatchDto batch) {
    if (_disposed) return;
    for (final sample in batch.samples) {
      recordHistory(sample);
    }
    if (batch.samples.isNotEmpty) {
      status = batch.samples.last;
      _markFresh(<String>[...statusTileKeys, keyRtt]);
      _clearError();
    }
    _trackSeqGap(batch.seq);
    notifyListeners();
  }

  /// Test seam feeding one batch through the production path.
  @visibleForTesting
  void feedBatch(frb.StatusBatchDto batch) => _applyBatch(batch);

  /// Counts missing batches from a monotonic [seq]: a jump of `n` adds `n - 1`.
  void _trackSeqGap(BigInt seq) {
    final previous = _lastSeq;
    if (previous != null && seq > previous + BigInt.one) {
      droppedBatches += (seq - previous - BigInt.one).toInt();
    }
    _lastSeq = seq;
  }

  /// Starts the 1 kHz status watch (or the injected [batch] stream in tests).
  /// A missing bridge binding is swallowed: the connection stays up and the
  /// tiles honestly degrade to `unknown` until the watch is available.
  void _startWatch() {
    _stopWatch();
    _lastSeq = null;
    droppedBatches = 0;
    try {
      final stream = batch ?? frb.startStatusWatch(rateHz: watchRateHz);
      _batchSub = stream.listen(
        _applyBatch,
        onError: (Object error) {
          _recordFailure(error);
          if (!_disposed) notifyListeners();
        },
      );
    } catch (_) {
      _batchSub = null;
    }
  }

  /// Stops the watch: cancels the subscription, then asks the device to stop
  /// streaming. Safe to call repeatedly.
  void _stopWatch() {
    _batchSub?.cancel();
    _batchSub = null;
    try {
      frb.stopStatusWatch();
    } catch (_) {
      // Bridge watch unavailable; nothing to stop.
    }
  }

  /// Latched fault bitmask reported alongside [status].
  int latchedFlags = 0;

  /// Whether the motor is currently running; false until a status is read.
  bool get motorRunning => status?.motorRunning ?? false;

  /// Number of latched fault bits currently reported by the device.
  int get latchedCount {
    var bits = latchedFlags;
    var count = 0;
    while (bits != 0) {
      count += bits & 1;
      bits >>= 1;
    }
    return count;
  }

  /// Trust level of the live value behind [key] (design-system §6 rule 2).
  ///
  /// [Freshness.unknown] until the value has ever been read, [Freshness.live]
  /// within [staleAfter] of its last update, and [Freshness.stale] once that
  /// window has elapsed. This is the single place the tri-state is decided, so
  /// a failed poll can never keep passing an old value off as current.
  Freshness freshnessOf(String key) {
    final at = _freshAt[key];
    if (at == null) return Freshness.unknown;
    return DateTime.now().difference(at) > kStaleAfter
        ? Freshness.stale
        : Freshness.live;
  }

  /// Age of the live value behind [key], or null when never read.
  Duration? staleFor(String key) {
    final at = _freshAt[key];
    return at == null ? null : DateTime.now().difference(at);
  }

  /// Status-tile keys that have aged out to [Freshness.stale] (a failed poll).
  /// A never-read tile is not listed — unknown is not the same as stale.
  List<String> get staleStatusTiles => statusTileKeys
      .where((key) => freshnessOf(key) == Freshness.stale)
      .toList();

  /// Test-only: forces the last-updated time of [key] so staleness can be
  /// exercised without waiting on the wall clock.
  @visibleForTesting
  void seedFreshness(String key, DateTime at) => _freshAt[key] = at;

  /// Records a measured STATUS_GET round-trip time [ms], smoothed with an EMA
  /// (α = [rttEmaAlpha]). The first sample is adopted directly.
  @visibleForTesting
  void recordRtt(double ms) => _updateRtt(ms);

  void _updateRtt(double ms) {
    final previous = rttMs;
    rttMs = previous == null ? ms : previous + (ms - previous) * rttEmaAlpha;
  }

  /// Marks every key in [keys] as freshly updated at [at] (defaults to now).
  void _markFresh(Iterable<String> keys, [DateTime? at]) {
    final stamp = at ?? DateTime.now();
    for (final key in keys) {
      _freshAt[key] = stamp;
    }
  }

  /// True when the device interlock would accept a COMMIT: the motor is
  /// stopped and no fault is latched. Mirrors the firmware rule — stop the
  /// drive and clear latched faults before flushing parameters to flash.
  bool get commitAllowed => !motorRunning && latchedFlags == 0;

  /// NTC over-temperature trip threshold [°C] (`software.fault.ntc_ot_c`).
  ///
  /// NOTE: the firmware does not currently expose this parameter — the NTC
  /// over-temperature bit is reserved in `app_fault.h` and neither
  /// `config/software.yaml` nor `app_fault.h` carries an `ntc_ot_c` key — so
  /// this getter is effectively always null and the UI honestly shows `—`.
  /// The PREVIEW's `▲ 接近门限 85` sample is illustrative only; no limit is
  /// fabricated here.
  ///
  /// Read from the already-loaded parameter table; null when that parameter
  /// has not been read yet.
  double? get thresholdNtc => _paramDouble('software.fault.ntc_ot_c');

  /// Bus over-voltage trip threshold [V] (`software.fault.vbus_ov_v`).
  double? get thresholdVbus => _paramDouble('software.fault.vbus_ov_v');

  /// Over-current trip threshold [A], preferring the motor's 10 s peak rating
  /// (`motor.i_peak_10s_a`) and falling back to `software.fault.oc_trip_a`
  /// when only that one is exposed. Null when neither is loaded.
  double? get thresholdCurrent =>
      _paramDouble('motor.i_peak_10s_a') ??
      _paramDouble('software.fault.oc_trip_a');

  /// Rotor-encoder gear ratio (`motor.encoder.rotor_ratio`), the factor from
  /// rotor angle to rotor-encoder angle; dimensionless. Null until the
  /// parameter table has been read, so the UI honestly shows `—`.
  ///
  /// NOTE: the wave brief writes this key as `motor.rotor_ratio`, but the
  /// parameter pipeline (`scripts/gen_params.py`) emits the nested name
  /// `motor.encoder.rotor_ratio`; the real wire key is used here so the ratio
  /// is never a fabricated `—`.
  double? get rotorRatio => _paramDouble('motor.encoder.rotor_ratio');

  /// Output-shaft encoder gear ratio (`motor.encoder.output_ratio`), the factor
  /// from rotor angle to output-encoder angle; dimensionless. Null until the
  /// parameter table has been read.
  double? get outputRatio => _paramDouble('motor.encoder.output_ratio');

  /// Rotor-encoder single-turn absolute resolution [bit]
  /// (`motor.encoder.resolution_bits`); the number of bits spanning one
  /// mechanical revolution. Null until the parameter table has been read, so
  /// the UI honestly shows `—` rather than a fabricated width.
  int? get encoderResolutionBits =>
      _paramDouble('motor.encoder.resolution_bits')?.round();

  /// Decodes parameter [name] to its physical float, or null when unloaded.
  double? _paramDouble(String name) {
    final value = values[name];
    if (value == null) return null;
    return switch (value.ty) {
      0 => (ByteData(4)..setUint32(0, value.raw, Endian.little)).getFloat32(
        0,
        Endian.little,
      ),
      1 || 2 || 3 => value.raw.toDouble(),
      _ => null,
    };
  }

  /// Overrides the bridge control calls when set (test injection). Receives
  /// the operation name (`start`/`stop`/`setFreq`/`setMod`/`faultClear`).
  Future<void> Function(String op)? onCtrl;

  /// Periodic [frb.statsGet] refresher; active only while connected.
  Timer? statusTimer;

  /// Test-only: when set, [connect] throws this instead of calling the bridge.
  @visibleForTesting
  Object? connectError;

  /// Test-only: overrides the bridge connect + PROTO_INFO handshake. Receives
  /// the requested address and returns the `(major, minor, caps, name)` tuple.
  @visibleForTesting
  Future<(int, int, int, String)> Function(String addr)? onConnect;

  /// Test-only: invoked in place of the bridge work when [loadMetas] runs, so
  /// tests can observe auto-load triggers without touching the bridge. Always
  /// null in production.
  @visibleForTesting
  void Function()? onLoadMetas;

  bool _disposed = false;

  /// Test-only constructor seeding [metas] and [status] without touching the
  /// bridge. [onCtrl], [onConnect] and [connectError] may be supplied here or
  /// assigned afterwards; the connection/session fields seed the top bar.
  @visibleForTesting
  AppState.test({
    List<frb.ParamMetaDto> metas = const [],
    List<frb.StatusDto> history = const [],
    this.status,
    this.onCtrl,
    this.connectError,
    this.onConnect,
    this.connected = false,
    this.deviceName = '',
    this.protoMajor = 0,
    this.protoMinor = 0,
    this.capabilities = 0,
    this.sessionUptime,
    this.attempts = 0,
    this.rttMs,
    this.batch,
  }) : metas = List.of(metas) {
    statusHistory.addAll(history);
    // Seed freshness from the injected snapshot so tri-state wiring is live in
    // tests: a supplied status means its tiles are current, and a connected
    // session means its counters are known. The stamp carries a small margin
    // because widget tests render after construction and can exceed the tight
    // [kStaleAfter]; production never uses this constructor.
    final stamp = DateTime.now().add(const Duration(seconds: 1));
    if (status != null) _markFresh(statusTileKeys, stamp);
    if (connected) {
      _markFresh(<String>[keyCrc, keyResync, keyRetry, keyFrames], stamp);
    }
    if (rttMs != null) _markFresh(<String>[keyRtt], stamp);
  }

  /// Loads the metadata table, then reads every parameter value. Exceptions are
  /// swallowed into [_recordFailure] so callers (notably the auto-load in
  /// [connect]) never see a throw.
  Future<void> loadMetas() async {
    final injected = onLoadMetas;
    if (injected != null) {
      injected();
      return;
    }
    loadingParams = true;
    notifyListeners();
    try {
      final n = await frb.paramMetaCount();
      final list = <frb.ParamMetaDto>[];
      for (var i = 0; i < n; i++) {
        list.add(await frb.paramMetaGet(index: i));
      }
      metas = list;
      await readAll();
    } on Exception catch (e) {
      _recordFailure(e);
    }
    loadingParams = false;
    notifyListeners();
  }

  /// Fire-and-forget parameter auto-load triggered by a successful [connect].
  ///
  /// [loadMetas] already records bridge [Exception]s, but a synchronous bridge
  /// state [Error] would otherwise escape an unawaited future and surface as an
  /// unhandled async error. Swallow anything remaining so the connection state
  /// is never disturbed, and keep polling alive.
  Future<void> _autoLoadMetas() async {
    try {
      await loadMetas();
    } catch (e) {
      _recordFailure(e);
      if (!_disposed) notifyListeners();
    }
  }

  /// Re-reads every parameter value into [values].
  Future<void> readAll() async {
    for (final m in metas) {
      try {
        values[m.name] = await frb.paramRead(name: m.name);
      } on Exception catch (e) {
        _recordFailure(e);
      }
    }
    notifyListeners();
  }

  /// Writes [raw] (in [ty] encoding) to [name]; marks the entry dirty.
  Future<bool> writeParam(String name, int ty, int raw) async {
    try {
      await frb.paramWrite(name: name, ty: ty, raw: raw);
      values[name] = frb.ParamValueDto(ty: ty, raw: raw);
      dirty.add(name);
      _clearError();
      notifyListeners();
      return true;
    } on Exception catch (e) {
      _recordFailure(e);
      notifyListeners();
      return false;
    }
  }

  /// Flushes dirty parameters to flash; clears [dirty] on success.
  Future<bool> commit() async {
    try {
      await frb.paramCommit();
      dirty.clear();
      _clearError();
      notifyListeners();
      return true;
    } on Exception catch (e) {
      _recordFailure(e);
      notifyListeners();
      return false;
    }
  }

  /// Runs a control [op] (through [onCtrl] when injected) and records any
  /// failure so the UI can classify it by [lastErrorCode].
  Future<bool> _ctrl(String op, Future<void> Function() fallback) async {
    try {
      final injected = onCtrl;
      if (injected != null) {
        await injected(op);
      } else {
        await fallback();
      }
      _clearError();
      notifyListeners();
      return true;
    } on Exception catch (e) {
      _recordFailure(e);
      notifyListeners();
      return false;
    }
  }

  /// Starts the motor. Returns false on failure (see [lastErrorCode]).
  Future<bool> ctrlStart() => _ctrl('start', frb.ctrlStart);

  /// Stops the motor. Returns false on failure (see [lastErrorCode]).
  Future<bool> ctrlStop() => _ctrl('stop', frb.ctrlStop);

  /// Sets the commanded frequency in Hz.
  Future<bool> ctrlSetFreq(double hz) =>
      _ctrl('setFreq', () => frb.ctrlSetFreq(freqHz: hz));

  /// Sets the modulation depth in percent.
  Future<bool> ctrlSetMod(double pct) =>
      _ctrl('setMod', () => frb.ctrlSetMod(modPct: pct));

  /// Clears latched faults. Returns false on failure.
  Future<bool> faultClear() => _ctrl('faultClear', frb.faultClear);

  /// Connects to [addr] and reads the device name from PROTO_INFO.
  /// Returns true on success; on failure [lastError] is populated.
  Future<bool> connect(String addr) async {
    try {
      final (int, int, int, String) info;
      final injected = onConnect;
      if (injected != null) {
        info = await injected(addr);
      } else {
        final injectedError = connectError;
        if (injectedError != null) throw injectedError;
        await frb.connect(addr: addr);
        info = await frb.protoInfo();
      }
      protoMajor = info.$1;
      protoMinor = info.$2;
      capabilities = info.$3;
      deviceName = info.$4;
      connected = true;
      sessionStart = DateTime.now();
      sessionUptime = Duration.zero;
      _clearError();
      _startPolling();
      _startWatch();
      // Fire-and-forget: the parameter table (thresholds, warn judgments) must
      // populate without the user pressing「读全部」. loadMetas owns its
      // loadingParams state and records failures, so a bridge error can never
      // escape into the connection state.
      unawaited(_autoLoadMetas());
      notifyListeners();
      return true;
    } catch (e) {
      _stopPolling();
      _stopWatch();
      _recordFailure(e);
      connected = false;
      notifyListeners();
      return false;
    }
  }

  Future<void> disconnect() async {
    _stopPolling();
    _stopWatch();
    try {
      await frb.disconnect();
    } catch (e) {
      _recordFailure(e);
    } finally {
      connected = false;
      deviceName = '';
      protoMajor = 0;
      protoMinor = 0;
      capabilities = 0;
      sessionStart = null;
      sessionUptime = null;
      attempts = 0;
      rttMs = null;
      status = null;
      latchedFlags = 0;
      statusHistory.clear();
      _lastSeq = null;
      droppedBatches = 0;
      _freshAt.clear();
      notifyListeners();
    }
  }

  void _startPolling() {
    _stopPolling();
    statusTimer = Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => _pollStats(),
    );
  }

  void _stopPolling() {
    statusTimer?.cancel();
    statusTimer = null;
  }

  /// Slow 500 ms poll kept only for the counters and the latched fault set.
  /// The live telemetry now arrives through the 1 kHz watch ([_startWatch]);
  /// the old single-shot `statusGet` path is gone. The `statsGet` round-trip
  /// is measured into [rttMs].
  Future<void> _pollStats() async {
    try {
      final started = DateTime.now();
      final stats = await frb.statsGet();
      _updateRtt(DateTime.now().difference(started).inMicroseconds / 1000.0);
      final fault = await frb.faultGet();
      if (_disposed) return;
      reconnects = stats.reconnects.toInt();
      crcErrors = stats.crcErrors.toInt();
      attempts = stats.attempts.toInt();
      latchedFlags = fault.$2;
      _markFresh(<String>[keyCrc, keyResync, keyRetry, keyFrames, keyRtt]);
      final start = sessionStart;
      if (start != null) {
        sessionUptime = DateTime.now().difference(start);
      }
      notifyListeners();
    } catch (e) {
      if (_disposed) return;
      _recordFailure(e);
      notifyListeners();
    }
  }

  @override
  void dispose() {
    _disposed = true;
    _stopPolling();
    _stopWatch();
    super.dispose();
  }

  /// Stores [error] and its device code for later UI classification.
  ///
  /// A concrete [frb.FrbError] is translated into the bridge-agnostic
  /// [GuiException] at this boundary. [frb.FrbError] has no `toString`
  /// override, so its [lastError] is rendered explicitly as
  /// `code=N: message` instead of `Instance of 'FrbError'`.
  void _recordError(Object error) {
    if (error is frb.FrbError) {
      final converted = GuiException(error.code, error.message);
      lastError = 'code=${converted.code}: ${converted.message}';
      lastErrorCode = converted.code;
      return;
    }
    if (error is GuiException) {
      lastError = error.message;
      lastErrorCode = error.code;
      return;
    }
    lastError = error.toString();
    lastErrorCode = exceptionCode(error);
  }

  /// Central failure sink for every bridge request: records [error] and counts
  /// it exactly once in [requestFailures].
  void _recordFailure(Object error) {
    _recordError(error);
    requestFailures++;
  }

  /// Clears the last error and its code.
  void _clearError() {
    lastError = '';
    lastErrorCode = -1;
  }
}

/// GUI-side carrier for a device error code, shielding the UI from the
/// concrete bridge exception type. UI code should branch on [code] only.
class GuiException implements Exception {
  const GuiException(this.code, this.message);

  final int code;
  final String message;

  @override
  String toString() => 'GuiException(code=$code, message=$message)';
}

/// Extracts the device-side error code from a bridge failure.
///
/// Prefers the typed [GuiException]/[frb.FrbError]; falls back to a `code: N`
/// scan of the exception text so wrapped/stringified errors still classify.
/// Returns -1 when no code can be recovered.
int exceptionCode(Object error) {
  if (error is GuiException) return error.code;
  if (error is frb.FrbError) return error.code;
  final match = RegExp(r'code[\s:=]+(\d+)').firstMatch(error.toString());
  return match == null ? -1 : int.parse(match.group(1)!);
}
