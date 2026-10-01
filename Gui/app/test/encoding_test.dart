import 'package:flutter_test/flutter_test.dart';

import 'package:g6618_gui/widgets/param_controls.dart';

void main() {
  test('f32ToRaw encodes 36.0 as the canonical IEEE-754 bit pattern', () {
    expect(f32ToRaw(36.0), 1108344832, reason: '0x42100000');
    expect(f32FromRaw(1108344832), closeTo(36.0, 1e-6));
  });

  test('negative f32 -1.0 round-trips through 0xBF800000', () {
    final raw = 0xBF800000.toSigned(32);
    expect(f32ToRaw(-1.0), raw);
    expect(f32FromRaw(raw), closeTo(-1.0, 1e-6));
  });

  test('f32 raw conversion round-trips across representative values', () {
    for (final v in <double>[0.0, 1.5, -3.25, 100.0, 480.0, -0.001]) {
      expect(f32FromRaw(f32ToRaw(v)), closeTo(v, 1e-6), reason: 'v=$v');
    }
  });

  test('display formatting renders canonical raw bit patterns', () {
    expect(f32FromRaw(1108344832).toStringAsFixed(1), '36.0');
    expect(formatValue(0, 1108344832), '36.00');
    expect(formatValue(3, 1), 'true');
  });

  test('i32 negative raw value renders signed and is not byte-swapped', () {
    expect(typeName(2), 'i32');
    expect(formatValue(2, -5), '-5');
  });
}
