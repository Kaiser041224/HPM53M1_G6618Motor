import 'package:flutter/foundation.dart';
import '../bridge/frb/api.dart' as frb;

/// App-wide observable state. This task covers connection + device name only;
/// parameter table, status snapshot and counters are added by later tasks.
class AppState extends ChangeNotifier {
  bool connected = false;
  String lastError = '';
  String deviceName = '';

  /// Connects to [addr] and reads the device name from PROTO_INFO.
  /// Returns true on success; on failure [lastError] is populated.
  Future<bool> connect(String addr) async {
    try {
      await frb.connect(addr: addr);
      final info = await frb.protoInfo();
      deviceName = info.$4;
      connected = true;
      lastError = '';
      notifyListeners();
      return true;
    } on Exception catch (e) {
      lastError = e.toString();
      connected = false;
      notifyListeners();
      return false;
    }
  }

  Future<void> disconnect() async {
    await frb.disconnect();
    connected = false;
    deviceName = '';
    notifyListeners();
  }
}
