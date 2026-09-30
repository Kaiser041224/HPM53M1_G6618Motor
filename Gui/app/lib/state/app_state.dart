import 'dart:async';

import 'package:flutter/foundation.dart';

import '../bridge/frb/api.dart' as frb;

// Re-exported so the UI layer depends only on AppState (ui -> state -> bridge)
// and never imports the generated bridge code directly.
export '../bridge/frb/api.dart'
    show ParamMetaDto, ParamValueDto, StatusDto, StatsDto;

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

  /// Elapsed time since [sessionStart], refreshed by the 500 ms status poll.
  /// Null while disconnected so the top bar can honestly show `—`.
  Duration? sessionUptime;

  /// Round-trip time of the last STATUS_GET, in milliseconds. Filled by the
  /// telemetry task; null until then so the UI shows `—`.
  double? rttMs;

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

  /// Last device status snapshot, refreshed by [refreshStatus].
  frb.StatusDto? status;

  /// Latched fault bitmask reported alongside [status].
  int latchedFlags = 0;

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

  bool _disposed = false;

  /// Test-only constructor seeding [metas] and [status] without touching the
  /// bridge. [onCtrl], [onConnect] and [connectError] may be supplied here or
  /// assigned afterwards; the connection/session fields seed the top bar.
  @visibleForTesting
  AppState.test({
    List<frb.ParamMetaDto> metas = const [],
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
  }) : metas = List.of(metas);

  /// Loads the metadata table, then reads every parameter value.
  Future<void> loadMetas() async {
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

  /// Refreshes [status] and [latchedFlags] from the device.
  Future<void> refreshStatus() async {
    try {
      status = await frb.statusGet();
      final fault = await frb.faultGet();
      latchedFlags = fault.$2;
      _clearError();
    } on Exception catch (e) {
      _recordFailure(e);
    }
    if (_disposed) return;
    notifyListeners();
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
      notifyListeners();
      return true;
    } catch (e) {
      _stopPolling();
      _recordFailure(e);
      connected = false;
      notifyListeners();
      return false;
    }
  }

  Future<void> disconnect() async {
    _stopPolling();
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
      status = null;
      latchedFlags = 0;
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

  Future<void> _pollStats() async {
    try {
      final stats = await frb.statsGet();
      if (_disposed) return;
      reconnects = stats.reconnects.toInt();
      crcErrors = stats.crcErrors.toInt();
      attempts = stats.attempts.toInt();
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
    await refreshStatus();
  }

  @override
  void dispose() {
    _disposed = true;
    _stopPolling();
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
