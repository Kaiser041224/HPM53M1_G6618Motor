import 'dart:async';

import 'package:flutter/foundation.dart';

import '../bridge/frb/api.dart' as frb;

/// App-wide observable state.
///
/// Holds the connection state plus the device-side statistics that the
/// persistent status bar shows. Parameter table, status snapshot and control
/// state are added by later tasks.
class AppState extends ChangeNotifier {
  bool connected = false;
  String lastError = '';
  String deviceName = '';

  /// Device-side counters, refreshed by the 500 ms status poll.
  int reconnects = 0;
  int crcErrors = 0;

  /// Client-side count of failed polls / requests.
  int requestFailures = 0;

  /// Periodic [frb.statsGet] refresher; active only while connected.
  Timer? statusTimer;

  bool _disposed = false;

  /// Connects to [addr] and reads the device name from PROTO_INFO.
  /// Returns true on success; on failure [lastError] is populated.
  Future<bool> connect(String addr) async {
    try {
      await frb.connect(addr: addr);
      final info = await frb.protoInfo();
      deviceName = info.$4;
      connected = true;
      lastError = '';
      _startPolling();
      notifyListeners();
      return true;
    } catch (e) {
      lastError = e.toString();
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
      lastError = e.toString();
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
}
