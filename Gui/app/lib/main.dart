import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:window_manager/window_manager.dart';

import 'bridge/frb/api.dart' as frb;
import 'bridge/frb/frb_generated.dart';
import 'state/app_state.dart';
import 'theme/gui_theme.dart';
import 'ui/single_page.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await _configureWindow();
  _registerFontLicenses();
  try {
    await RustLib.init();
    // FFI smoke: resolves native symbols without needing a device, so a broken
    // bridge fails here instead of on the first user action.
    await frb.isConnected();
    runApp(const GuiApp());
  } catch (error) {
    runApp(BridgeStartupErrorApp(error: error.toString()));
  }
}

/// Hides the native title bar and shows the window once Flutter is ready.
///
/// `TitleBarStyle.hidden` only custom-draws the title area away — it does not
/// strip `WS_THICKFRAME`, so the native frame, drop shadow and edge resize stay
/// intact; `setResizable(true)` makes that explicit. The caption cluster in
/// [TopBar] replaces the hidden native buttons. A failure here (unsupported
/// platform) is non-fatal: the app still starts with its native chrome.
Future<void> _configureWindow() async {
  try {
    await windowManager.ensureInitialized();
    const windowOptions = WindowOptions(
      size: Size(1280, 800),
      minimumSize: Size(1024, 640),
      center: true,
      title: 'Gui',
      titleBarStyle: TitleBarStyle.hidden,
    );
    await windowManager.waitUntilReadyToShow(windowOptions, () async {
      await windowManager.show();
      await windowManager.focus();
    });
    await windowManager.setResizable(true);
  } catch (error) {
    debugPrint('window setup skipped: $error');
  }
}

/// Registers the SIL OFL 1.1 license texts bundled with the Electrolize and
/// Aldrich display fonts so `showLicensePage` can display them.
void _registerFontLicenses() {
  LicenseRegistry.addLicense(() async* {
    yield LicenseEntryWithLineBreaks(
      const <String>['Electrolize'],
      await rootBundle.loadString('assets/fonts/OFL-Electrolize.txt'),
    );
    yield LicenseEntryWithLineBreaks(
      const <String>['Aldrich'],
      await rootBundle.loadString('assets/fonts/OFL-Aldrich.txt'),
    );
  });
}

/// Root widget. Owns the single [AppState] instance and builds the
/// [SinglePage] (top bar + flat zones + status bar).
class GuiApp extends StatefulWidget {
  const GuiApp({super.key});

  @override
  State<GuiApp> createState() => _GuiAppState();
}

/// Minimal fallback shown when the Rust bridge cannot be initialized at
/// startup. The usual cause is a missing/blocked `gui_frb.dll`, so the raw
/// exception is surfaced alongside that hint.
class BridgeStartupErrorApp extends StatelessWidget {
  const BridgeStartupErrorApp({super.key, required this.error});

  final String error;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Gui',
      debugShowCheckedModeBanner: false,
      theme: buildGuiTheme(),
      home: Scaffold(
        body: Center(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                const Icon(Icons.error_outline, size: 48),
                const SizedBox(height: 16),
                const Text('Rust 桥接初始化失败'),
                const SizedBox(height: 8),
                const Text('请检查 gui_frb.dll 是否与可执行文件位于同一目录。'),
                const SizedBox(height: 16),
                SelectableText(error, textAlign: TextAlign.center),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _GuiAppState extends State<GuiApp> {
  late final AppState _state = AppState();

  /// Native/Material window title: `Gui`, or `Gui · <device>` once the
  /// PROTO_INFO handshake has named the device.
  String _windowTitle = 'Gui';

  @override
  void initState() {
    super.initState();
    _state.addListener(_syncWindowTitle);
  }

  @override
  void dispose() {
    _state.removeListener(_syncWindowTitle);
    _state.dispose();
    super.dispose();
  }

  /// Pushes the state-derived title to the native window. The platform channel
  /// only exists in a real desktop window; widget tests have none, so a failed
  /// call is expected and ignored.
  void _syncWindowTitle() {
    final title = _state.connected && _state.deviceName.isNotEmpty
        ? 'Gui · ${_state.deviceName}'
        : 'Gui';
    if (title == _windowTitle) return;
    _windowTitle = title;
    windowManager.setTitle(title).catchError((Object _) {});
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _state,
      builder: (context, _) => MaterialApp(
        title: _windowTitle,
        debugShowCheckedModeBanner: false,
        theme: buildGuiTheme(),
        home: SinglePage(state: _state),
      ),
    );
  }
}
