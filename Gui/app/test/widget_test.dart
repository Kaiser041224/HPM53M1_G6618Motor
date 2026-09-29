import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/main.dart';
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/ui/theme.dart';

void main() {
  testWidgets('shell renders the Gui placeholder', (WidgetTester tester) async {
    await tester.pumpWidget(const GuiApp());

    expect(find.textContaining('Gui — bridge ok: false'), findsOneWidget);

    final app = tester.widget<MaterialApp>(find.byType(MaterialApp));
    expect(app.theme?.useMaterial3, isTrue);
    expect(app.theme?.colorScheme.brightness, Brightness.dark);
  });

  test('AppState starts disconnected with empty device name', () {
    final state = AppState();
    expect(state.connected, isFalse);
    expect(state.deviceName, isEmpty);
    expect(state.lastError, isEmpty);
  });

  test('buildGuiTheme is Material 3 dark', () {
    final theme = buildGuiTheme();
    expect(theme.useMaterial3, isTrue);
    expect(theme.colorScheme.brightness, Brightness.dark);
  });
}
