import 'dart:async';

import 'package:flutter/foundation.dart';

import '../bridge/frb/api.dart' as frb;

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

  /// Device-side counters, refreshed by the 500 ms status poll.
  int reconnects = 0;
  int crcErrors = 0;

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

  /// Periodic [frb.statsGet] refresher; active only while connected.
  Timer? statusTimer;

  bool _disposed = false;

  /// Test-only constructor seeding [metas] without touching the bridge.
  @visibleForTesting
  AppState.test({required List<frb.ParamMetaDto> metas}) {
    this.metas = List.of(metas);
  }

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
      _recordError(e);
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
        _recordError(e);
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
      _recordError(e);
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
      _recordError(e);
      notifyListeners();
      return false;
    }
  }

  /// Connects to [addr] and reads the device name from PROTO_INFO.
  /// Returns true on success; on failure [lastError] is populated.
  Future<bool> connect(String addr) async {
    try {
      await frb.connect(addr: addr);
      final info = await frb.protoInfo();
      deviceName = info.$4;
      connected = true;
      _clearError();
      _startPolling();
      notifyListeners();
      return true;
    } catch (e) {
      _recordError(e);
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
      _recordError(e);
    } finally {
      connected = false;
      deviceName = '';
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
      notifyListeners();
    } catch (_) {
      if (_disposed) return;
      requestFailures++;
      notifyListeners();
    }
  }

  @override
  void dispose() {
    _disposed = true;
    _stopPolling();
    super.dispose();
  }

  /// Stores [error] and its device code for later UI classification.
  void _recordError(Object error) {
    lastError = error.toString();
    lastErrorCode = exceptionCode(error);
  }

  /// Clears the last error and its code.
  void _clearError() {
    lastError = '';
    lastErrorCode = -1;
  }
}

/// Extracts the device-side error code from a bridge failure.
///
/// Prefers the typed [frb.FrbError]; falls back to a `code: N` scan of the
/// exception text so wrapped/stringified errors still classify. Returns -1
/// when no code can be recovered.
int exceptionCode(Object error) {
  if (error is frb.FrbError) return error.code;
  final match = RegExp(r'code[\s:=]+(\d+)').firstMatch(error.toString());
  return match == null ? -1 : int.parse(match.group(1)!);
}
