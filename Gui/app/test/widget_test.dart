import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/main.dart';
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/single_page.dart';
import 'package:g6618_gui/ui/status_bar.dart';
import 'package:g6618_gui/ui/top_bar.dart';

void main() {
  testWidgets('single page renders top bar, zones and persistent status bar', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const GuiApp());

    expect(find.byType(SinglePage), findsOneWidget);
    expect(find.byType(TopBar), findsOneWidget);
    expect(find.byType(StatusBar), findsOneWidget);
    expect(find.text('DEVICE ID PENDING'), findsOneWidget);

    final app = tester.widget<MaterialApp>(find.byType(MaterialApp));
    expect(app.theme?.useMaterial3, isTrue);
    expect(app.theme?.colorScheme.brightness, Brightness.dark);
  });

  test('AppState starts disconnected with zeroed counters', () {
    final state = AppState();
    expect(state.connected, isFalse);
    expect(state.deviceName, isEmpty);
    expect(state.lastError, isEmpty);
    expect(state.reconnects, 0);
    expect(state.crcErrors, 0);
    expect(state.requestFailures, 0);
    expect(state.statusTimer, isNull);
    expect(state.sessionUptime, isNull);
    expect(state.attempts, 0);
  });

  test('buildGuiTheme is Material 3 dark', () {
    final theme = buildGuiTheme();
    expect(theme.useMaterial3, isTrue);
    expect(theme.colorScheme.brightness, Brightness.dark);
  });
}
