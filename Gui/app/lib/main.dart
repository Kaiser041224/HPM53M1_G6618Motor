import 'package:flutter/material.dart';

import 'bridge/frb/api.dart' as frb;
import 'bridge/frb/frb_generated.dart';
import 'state/app_state.dart';
import 'theme/gui_theme.dart';
import 'ui/single_page.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
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

  @override
  void dispose() {
    _state.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _state,
      builder: (context, _) => MaterialApp(
        title: 'Gui',
        debugShowCheckedModeBanner: false,
        theme: buildGuiTheme(),
        home: SinglePage(state: _state),
      ),
    );
  }
}
