import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/device_art_data.dart';
import 'package:g6618_gui/widgets/device_ascii.dart';
import 'package:g6618_gui/widgets/segment_bar.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';
import 'package:g6618_gui/widgets/status_badge.dart';

Future<void> _pump(WidgetTester tester, Widget child) {
  return tester.pumpWidget(
    MaterialApp(theme: buildGuiTheme(), home: Scaffold(body: child)),
  );
}

List<Color?> _segmentColors(WidgetTester tester) {
  return tester
      .widgetList<Container>(
        find.descendant(
          of: find.byType(SegmentBar),
          matching: find.byType(Container),
        ),
      )
      .map((c) => (c.decoration as BoxDecoration?)?.color)
      .toList();
}

void main() {
  group('SegmentBar', () {
    const palette = GuiPalette();

    testWidgets('renders one segment per requested count', (tester) async {
      await _pump(tester, const SegmentBar(value01: 1, segments: 10));
      expect(_segmentColors(tester).length, 10);
    });

    testWidgets('lights round(value*segments) segments in accent', (
      tester,
    ) async {
      await _pump(tester, const SegmentBar(value01: 0.5, segments: 10));
      final colors = _segmentColors(tester);
      expect(colors.where((c) => c == palette.accent).length, 5);
      expect(colors.where((c) => c == palette.bgRaise).length, 5);
    });

    testWidgets('warn swaps the lit color to accent2, never accent', (
      tester,
    ) async {
      await _pump(
        tester,
        const SegmentBar(value01: 1, segments: 4, warn: true),
      );
      final colors = _segmentColors(tester);
      expect(colors.where((c) => c == palette.accent2).length, 4);
      expect(colors.where((c) => c == palette.accent).length, 0);
    });

    testWidgets('uses 9px segments with 2px gaps', (tester) async {
      await _pump(tester, const SegmentBar(value01: 0.5, segments: 8));
      final boxes = tester.widgetList<SizedBox>(
        find.descendant(
          of: find.byType(SegmentBar),
          matching: find.byType(SizedBox),
        ),
      );
      expect(boxes.any((b) => b.height == 9), isTrue);
      expect(boxes.where((b) => b.width == 2).length, 7);
    });

    testWidgets('lit segments carry one weak same-hue glow (v3)', (tester) async {
      await _pump(tester, const SegmentBar(value01: 1, segments: 3));
      final lit = tester.widget<Container>(
        find
            .descendant(
              of: find.byType(SegmentBar),
              matching: find.byType(Container),
            )
            .first,
      );
      final shadow = (lit.decoration as BoxDecoration).boxShadow!.single;
      expect(
        shadow.color.toARGB32(),
        palette.accent.withValues(alpha: 0.22).toARGB32(),
      );
      expect(shadow.blurRadius, 8);
    });

    test('lit count rounds and clamps to [0, segments]', () {
      expect(SegmentBar.litCount(-0.5, 10), 0);
      expect(SegmentBar.litCount(2, 10), 10);
      expect(SegmentBar.litCount(0.44, 10), 4);
      expect(SegmentBar.litCount(0.45, 10), 5);
    });
  });

  group('StatusBadge', () {
    const palette = GuiPalette();

    final cases = <ItemState, String>{
      ItemState.normal: '✓',
      ItemState.warn: '!',
      ItemState.fault: '✖',
      ItemState.latched: '⚠',
      ItemState.stale: '∅',
      ItemState.pending: '…',
    };

    final colors = <ItemState, Color>{
      ItemState.normal: palette.stNormal,
      ItemState.warn: palette.stWarn,
      ItemState.fault: palette.stFault,
      ItemState.latched: palette.stLatched,
      ItemState.stale: palette.stStale,
      ItemState.pending: palette.stPending,
    };

    for (final entry in cases.entries) {
      testWidgets('${entry.key.name} carries icon + label + flat status color', (
        tester,
      ) async {
        await _pump(
          tester,
          StatusBadge(state: entry.key, label: 'LBL'),
        );
        expect(find.text(entry.value), findsOneWidget);
        expect(find.text('LBL'), findsOneWidget);

        final icon = tester.widget<Text>(find.text(entry.value));
        expect(icon.style?.color, colors[entry.key]);
        // v3 badges are flat: color + icon + text, no glow.
        expect(icon.style?.shadows, anyOf(isNull, isEmpty));
      });
    }

    testWidgets('uppercases the label (design-system §5)', (tester) async {
      await _pump(
        tester,
        const StatusBadge(state: ItemState.normal, label: 'fault active'),
      );
      expect(find.text('FAULT ACTIVE'), findsOneWidget);
      expect(find.text('fault active'), findsNothing);
    });
  });

  group('StaleGate', () {
    const palette = GuiPalette();

    testWidgets('live shows the live value even when it is zero', (
      tester,
    ) async {
      await _pump(
        tester,
        const StaleGate(freshness: Freshness.live, live: Text('0')),
      );
      expect(find.text('0'), findsOneWidget);
      expect(find.text('—'), findsNothing);
      expect(find.byType(DashedFrame), findsNothing);
    });

    testWidgets('stale replaces the value with a dashed em-dash', (
      tester,
    ) async {
      await _pump(
        tester,
        const StaleGate(freshness: Freshness.stale, live: Text('24.0')),
      );
      expect(find.text('24.0'), findsNothing);
      expect(find.text('—'), findsOneWidget);
      final frame = tester.widget<DashedFrame>(find.byType(DashedFrame));
      expect(frame.color, palette.stStale);
    });

    testWidgets('unknown also falls back, in the muted color', (tester) async {
      await _pump(
        tester,
        const StaleGate(freshness: Freshness.unknown, live: Text('24.0')),
      );
      expect(find.text('—'), findsOneWidget);
      final frame = tester.widget<DashedFrame>(find.byType(DashedFrame));
      expect(frame.color, palette.fgMute);
    });

    testWidgets('stale fallback carries no glow', (tester) async {
      await _pump(
        tester,
        const StaleGate(freshness: Freshness.stale, live: Text('24.0')),
      );
      final dash = tester.widget<Text>(find.text('—'));
      expect(dash.style?.shadows, anyOf(isNull, isEmpty));
    });
  });

  group('DeviceAscii', () {
    testWidgets('disconnected shows the pending placeholder', (tester) async {
      await _pump(tester, const DeviceAscii(connected: false));
      expect(find.text('DEVICE ID PENDING'), findsOneWidget);
      expect(find.byType(DashedFrame), findsOneWidget);
    });

    testWidgets('connected renders five rows of block art', (tester) async {
      await _pump(tester, const DeviceAscii(connected: true));
      expect(find.text('DEVICE ID PENDING'), findsNothing);
      final rows = tester
          .widgetList<Text>(
            find.descendant(
              of: find.byType(DeviceAscii),
              matching: find.byType(Text),
            ),
          )
          .where((t) => t.data?.contains('█') ?? false)
          .toList();
      expect(rows.length, artLines.length);
      for (final line in artLines) {
        expect(rows.any((t) => t.data == line), isTrue);
      }
    });

    testWidgets('connected shows the model name when provided', (tester) async {
      await _pump(
        tester,
        const DeviceAscii(connected: true, modelName: 'G6618 Motor Controller'),
      );
      expect(find.text('G6618 Motor Controller'), findsOneWidget);
    });

    testWidgets('art rows use the five stop colors, in order', (tester) async {
      await _pump(tester, const DeviceAscii(connected: true));
      for (var i = 0; i < artLines.length; i++) {
        expect(
          find.byWidgetPredicate(
            (w) =>
                w is Text &&
                w.data == artLines[i] &&
                w.style?.color == artColorStops[i],
          ),
          findsOneWidget,
        );
      }
    });
  });

  group('device art data', () {
    test('extracted art is five rows of 38 block-character columns', () {
      expect(artLines.length, 5);
      for (final line in artLines) {
        expect(line.length, 38);
        expect(line.contains('█'), isTrue);
      }
    });

    test('color stops follow the firmware 256-color order 45..213', () {
      expect(artColorStops, const <Color>[
        Color(0xFF00D7FF),
        Color(0xFF00FFFF),
        Color(0xFFAF87FF),
        Color(0xFFFF5FFF),
        Color(0xFFFF87FF),
      ]);
    });
  });

  testWidgets('device art rows carry a same-hue glow', (tester) async {
    await _pump(tester, const DeviceAscii(connected: true));
    final row = tester.widget<Text>(
      find.byWidgetPredicate(
        (w) =>
            w is Text &&
            w.data == artLines.first &&
            w.style?.color == artColorStops.first,
      ),
    );
    final shadows = row.style?.shadows ?? const <Shadow>[];
    expect(shadows, isNotEmpty);
    for (final shadow in shadows) {
      expect(shadow.color.r, closeTo(artColorStops.first.r, 0.001));
    }
  });
}
