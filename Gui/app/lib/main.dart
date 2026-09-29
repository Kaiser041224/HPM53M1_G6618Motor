import 'package:flutter/material.dart';

import 'bridge/frb/api.dart' as frb;
import 'bridge/frb/frb_generated.dart';
import 'state/app_state.dart';
import 'ui/theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await RustLib.init();
  final bridgeOk = await frb.isConnected();
  runApp(GuiApp(bridgeOk: bridgeOk));
}

/// Root widget. The `Shell` (navigation rail + status bar) replaces the
/// placeholder body in Task 4; `AppState` is the single source of app state.
///
/// `bridgeOk` is the startup FFI smoke result: `frb.isConnected()` touches the
/// native symbol table without needing a device, so a broken wire fails loudly.
class GuiApp extends StatelessWidget {
  const GuiApp({super.key, this.bridgeOk = false});

  final bool bridgeOk;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: AppState(),
      builder: (context, _) => MaterialApp(
        title: 'Gui',
        debugShowCheckedModeBanner: false,
        theme: buildGuiTheme(),
        home: Scaffold(
          body: Center(child: Text('Gui — bridge ok: $bridgeOk')),
        ),
      ),
    );
  }
}
