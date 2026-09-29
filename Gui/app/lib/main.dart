import 'package:flutter/material.dart';

import 'bridge/frb/api.dart' as frb;
import 'bridge/frb/frb_generated.dart';
import 'state/app_state.dart';
import 'ui/shell.dart';
import 'ui/theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await RustLib.init();
  final bridgeOk = await frb.isConnected();
  runApp(GuiApp(bridgeOk: bridgeOk));
}

/// Root widget. Owns the single [AppState] instance and builds the [Shell]
/// (navigation rail + status bar).
///
/// [bridgeOk] is the startup FFI smoke result: `frb.isConnected()` touches the
/// native symbol table without needing a device, so a broken wire fails loudly.
class GuiApp extends StatefulWidget {
  const GuiApp({super.key, this.bridgeOk = false});

  final bool bridgeOk;

  @override
  State<GuiApp> createState() => _GuiAppState();
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
        home: Shell(state: _state),
      ),
    );
  }
}
