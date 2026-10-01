import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// 断言全站提示已改用 `lib/ui/toast_overlay.dart` 的 overlay 提示，
/// `lib/` 下不再残留 SnackBar / ScaffoldMessenger 依赖。
void main() {
  test('lib/ 下不再有任何 SnackBar / ScaffoldMessenger 依赖', () {
    final root = Directory('lib');
    expect(root.existsSync(), isTrue, reason: '应在工程根目录运行测试');

    // 只匹配「代码形态」的调用点，避免文档注释里提及旧 API 造成误报。
    const patterns = <String>[
      'showSnackBar(',
      'ScaffoldMessenger.of(',
      'SnackBar(',
    ];
    final hits = <String>[];
    for (final entity in root.listSync(recursive: true)) {
      if (entity is! File || !entity.path.endsWith('.dart')) continue;
      final source = entity.readAsStringSync();
      for (final pattern in patterns) {
        if (source.contains(pattern)) {
          hits.add('${entity.path}: $pattern');
        }
      }
    }
    expect(hits, isEmpty, reason: 'SnackBar 调用点应全部迁移到 toast：$hits');
  });
}
