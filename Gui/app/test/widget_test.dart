import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/main.dart';
import 'package:g6618_gui/state/app_state.dart';
import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/ui/bottom_bar.dart';
import 'package:g6618_gui/ui/encoder_zone.dart';
import 'package:g6618_gui/ui/single_page.dart';
import 'package:g6618_gui/ui/top_bar.dart';

void main() {
  testWidgets('single page renders top bar, zones and persistent bottom bar', (
    WidgetTester tester,
  ) async {
    // The shell targets a desktop window (minimumSize 1024x640); render at the
    // default 1280x800 so the smoke test exercises a supported size.
    tester.view.physicalSize = const Size(1280, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(const GuiApp());

    expect(find.byType(SinglePage), findsOneWidget);
    expect(find.byType(TopBar), findsOneWidget);
    // The encoder zone is stacked on the flat monitor page, so it is built and
    // on stage by default.
    expect(find.byType(EncoderZone), findsOneWidget);
    expect(find.byType(BottomBar), findsOneWidget);
    expect(find.text('DEVICE ID PENDING'), findsOneWidget);

    final app = tester.widget<MaterialApp>(find.byType(MaterialApp));
    expect(app.theme?.useMaterial3, isTrue);
    expect(app.theme?.colorScheme.brightness, Brightness.dark);
  });

  testWidgets('整窗青色勾边：根容器 1.5px accent 直角外框包裹 SinglePage', (
    WidgetTester tester,
  ) async {
    tester.view.physicalSize = const Size(1280, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(const GuiApp());

    final frame = tester.widget<Container>(
      find.byKey(const Key('window_frame')),
    );
    final deco = frame.decoration! as BoxDecoration;
    const palette = GuiPalette();
    final border = deco.border! as Border;
    for (final side in <BorderSide>[
      border.top,
      border.right,
      border.bottom,
      border.left,
    ]) {
      expect(side.color, palette.accent);
      expect(side.width, 1.5);
    }
    expect(deco.color, palette.bgVoid);
    expect(deco.borderRadius, isNull, reason: '直角');
    expect(
      find.descendant(
        of: find.byKey(const Key('window_frame')),
        matching: find.byType(SinglePage),
      ),
      findsOneWidget,
    );
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
