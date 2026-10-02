import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/theme/gui_theme.dart';
import 'package:g6618_gui/widgets/device_art_data.dart';
import 'package:g6618_gui/widgets/device_ascii.dart';
import 'package:g6618_gui/widgets/glow_value.dart';
import 'package:g6618_gui/widgets/segment_bar.dart';
import 'package:g6618_gui/widgets/stale_gate.dart';
import 'package:g6618_gui/widgets/status_badge.dart';
import 'package:g6618_gui/widgets/vfd_bar_painter.dart';

Future<void> _pump(WidgetTester tester, Widget child) {
  return tester.pumpWidget(
    MaterialApp(theme: buildGuiTheme(), home: Scaffold(body: child)),
  );
}

void main() {
  group('SegmentBar（VFD 竖排发光管条语汇）', () {
    const palette = GuiPalette();

    testWidgets('渲染 VfdBar：ratio=value01、青色 tint、无门限刻度', (tester) async {
      await _pump(tester, const SegmentBar(value01: 0.5));

      final bar = tester.widget<VfdBar>(find.byType(VfdBar));
      expect(bar.ratio, 0.5);
      expect(bar.tint, palette.accent);
      expect(bar.warnBand, isFalse);
      expect(bar.peak, isNull);
    });

    testWidgets('warn 把 tint 换成警告橙', (tester) async {
      await _pump(tester, const SegmentBar(value01: 1, warn: true));

      expect(
        tester.widget<VfdBar>(find.byType(VfdBar)).tint,
        palette.valueOrange,
      );
    });

    testWidgets('显式 tint 覆盖 warn 默认', (tester) async {
      await _pump(
        tester,
        SegmentBar(value01: 1, warn: true, tint: palette.valuePurple),
      );

      expect(
        tester.widget<VfdBar>(find.byType(VfdBar)).tint,
        palette.valuePurple,
      );
    });

    test('litCount 保留（兼容）', () {
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

    testWidgets('connected paints a 5 x 38 cell grid', (tester) async {
      await _pump(tester, const DeviceAscii(connected: true));
      expect(find.text('DEVICE ID PENDING'), findsNothing);
      final paint = tester.widget<CustomPaint>(
        find.descendant(
          of: find.byType(DeviceAscii),
          matching: find.byType(CustomPaint),
        ),
      );
      final painter = paint.painter! as DeviceArtPainter;
      expect(painter.rows.length, 5);
      for (final row in painter.rows) {
        expect(row.length, 38);
      }
    });

    testWidgets('connected grid has solid cells on every row', (tester) async {
      await _pump(tester, const DeviceAscii(connected: true));
      final paint = tester.widget<CustomPaint>(
        find.descendant(
          of: find.byType(DeviceAscii),
          matching: find.byType(CustomPaint),
        ),
      );
      final painter = paint.painter! as DeviceArtPainter;
      final totals = painter.rows
          .map((row) => row.where((c) => c != ArtCell.empty).length)
          .toList();
      expect(totals.length, 5);
      expect(totals.every((n) => n > 0), isTrue);
    });

    testWidgets('art grid uses the five stop colors, in order', (tester) async {
      await _pump(tester, const DeviceAscii(connected: true));
      final paint = tester.widget<CustomPaint>(
        find.descendant(
          of: find.byType(DeviceAscii),
          matching: find.byType(CustomPaint),
        ),
      );
      final painter = paint.painter! as DeviceArtPainter;
      expect(painter.stops, artColorStops);
    });

    testWidgets('art glow keeps the ascii same-hue tiers', (tester) async {
      final shadows = DeviceArtPainter.glowFor(artColorStops.first);
      expect(shadows.length, greaterThan(1));
      for (final shadow in shadows) {
        expect(shadow.color.r, closeTo(artColorStops.first.r, 0.001));
        expect(shadow.color.g, closeTo(artColorStops.first.g, 0.001));
        expect(shadow.color.b, closeTo(artColorStops.first.b, 0.001));
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

    test('cell grid is 5 rows x 38 columns with solid counts per row', () {
      expect(artRows.length, 5);
      final counts = <int>[];
      for (final row in artRows) {
        expect(row.length, 38);
        counts.add(row.where((c) => c == ArtCell.full).length);
      }
      // Extracted from the firmware banner: solid `█` count per row.
      expect(counts, <int>[25, 13, 26, 18, 25]);
      expect(counts.every((n) => n > 0), isTrue);
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

  group('GlowValue 量纲 tint 透传', () {
    const palette = GuiPalette();

    testWidgets('显式 tint 覆盖 tone 颜色', (tester) async {
      await _pump(
        tester,
        GlowValue(
          value: 1.0,
          unit: 'V',
          precision: 1,
          tint: palette.valueCyan,
        ),
      );
      expect(
        tester.widget<Text>(find.text('1.0')).style?.color,
        palette.valueCyan,
      );
    });

    testWidgets('未给 tint 时回落 tone', (tester) async {
      await _pump(
        tester,
        const GlowValue(value: 1.0, precision: 1, tone: Tone.green),
      );
      expect(
        tester.widget<Text>(find.text('1.0')).style?.color,
        palette.valueGreen,
      );
    });

    testWidgets('null 值仍走 fgMute，不受 tint 影响', (tester) async {
      await _pump(
        tester,
        GlowValue(value: null, unit: 'V', tint: palette.valueCyan),
      );
      expect(tester.widget<Text>(find.text('—')).style?.color, palette.fgMute);
    });
  });
}
