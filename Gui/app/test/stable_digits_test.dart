import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/widgets/stable_digits.dart';

Future<void> _pump(WidgetTester tester, Widget child) {
  return tester.pumpWidget(
    MaterialApp(home: Scaffold(body: Center(child: child))),
  );
}

void main() {
  test('kTextRefreshHz is 5 Hz, i.e. a 200 ms period', () {
    expect(kTextRefreshHz, 5);
    expect(kTextRefreshPeriod, const Duration(milliseconds: 200));
  });

  testWidgets('shows the initial value immediately', (tester) async {
    await _pump(tester, const StableDigits(text: '1.0'));

    expect(find.text('1.0'), findsOneWidget);
  });

  testWidgets('holds the previous value for the first 199 ms', (tester) async {
    await _pump(tester, const StableDigits(text: '1.0'));
    await _pump(tester, const StableDigits(text: '2.0'));

    await tester.pump(const Duration(milliseconds: 199));

    expect(find.text('2.0'), findsNothing);
    expect(find.text('1.0'), findsOneWidget);
  });

  testWidgets('accepts the new value on the 200 ms tick', (tester) async {
    await _pump(tester, const StableDigits(text: '1.0'));
    await _pump(tester, const StableDigits(text: '2.0'));

    await tester.pump(const Duration(milliseconds: 200));

    expect(find.text('2.0'), findsOneWidget);
    expect(find.text('1.0'), findsNothing);
  });

  testWidgets('keeps the same style across a throttled transition', (
    tester,
  ) async {
    const style = TextStyle(fontSize: 12, color: Color(0xFF00FF00));
    await _pump(tester, const StableDigits(text: '1.0', style: style));
    await _pump(tester, const StableDigits(text: '2.0', style: style));

    await tester.pump(const Duration(milliseconds: 199));
    final held = tester.widget<Text>(find.text('1.0'));
    expect(held.style, style);

    await tester.pump(const Duration(milliseconds: 1));
    final updated = tester.widget<Text>(find.text('2.0'));
    expect(updated.style, style);
  });

  testWidgets('builder subtree only rebuilds on an accepted update', (
    tester,
  ) async {
    var builds = 0;
    Widget frame(String value) => StableDigits(
      text: value,
      builder: (context) {
        builds++;
        return Text(value);
      },
    );

    await _pump(tester, frame('1'));
    expect(builds, 1);

    // 25 Hz parent churn: four rebuilds inside the first 200 ms window.
    for (var i = 0; i < 4; i++) {
      await tester.pump(const Duration(milliseconds: 40));
      await _pump(tester, frame('${i + 2}'));
    }
    expect(builds, 1, reason: 'no rebuild before the 200 ms tick');
    expect(find.text('1'), findsOneWidget);

    await tester.pump(const Duration(milliseconds: 40));
    expect(builds, 2, reason: 'one rebuild on the tick');
    expect(find.text('5'), findsOneWidget);
  });

  group('StableRefresh 通用节流', () {
    testWidgets('初始立即构建', (tester) async {
      var builds = 0;
      await _pump(
        tester,
        StableRefresh(
          refreshKey: 'a',
          builder: (context) {
            builds++;
            return const Text('a');
          },
        ),
      );
      expect(builds, 1);
      expect(find.text('a'), findsOneWidget);
    });

    testWidgets('<200ms 不重建，≥200ms 才重建', (tester) async {
      var builds = 0;
      Widget frame(String key) => StableRefresh(
        refreshKey: key,
        builder: (context) {
          builds++;
          return Text(key);
        },
      );

      await _pump(tester, frame('a'));
      expect(builds, 1);

      await _pump(tester, frame('b'));
      await tester.pump(const Duration(milliseconds: 199));
      expect(builds, 1, reason: '199 ms < 200 ms tick');
      expect(find.text('a'), findsOneWidget);

      await tester.pump(const Duration(milliseconds: 1));
      expect(builds, 2, reason: '200 ms tick rebuilds the subtree');
      expect(find.text('b'), findsOneWidget);
    });

    testWidgets('key 不变时不重建', (tester) async {
      var builds = 0;
      Widget frame() => StableRefresh(
        refreshKey: 'same',
        builder: (context) {
          builds++;
          return const Text('same');
        },
      );

      await _pump(tester, frame());
      await _pump(tester, frame());
      await tester.pump(const Duration(milliseconds: 400));
      expect(builds, 1);
    });
  });
}
