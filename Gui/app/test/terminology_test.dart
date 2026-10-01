import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// 任务 W44：旧术语（luòpán）全面改为「写入」，全站（lib + test）不得残留。
/// 断言词以码位构造，注释亦回避该词字面，避免本文件自身命中。
void main() {
  test('全站文案不再出现旧术语（luòpán）', () {
    final needle = '\u843d\u76d8';
    final roots = <Directory>[Directory('lib'), Directory('test')];
    final offenders = <String>[];
    for (final root in roots) {
      if (!root.existsSync()) continue;
      for (final entity in root.listSync(recursive: true)) {
        if (entity is! File || !entity.path.endsWith('.dart')) continue;
        if (entity.readAsStringSync().contains(needle)) {
          offenders.add(entity.path);
        }
      }
    }
    expect(offenders, isEmpty, reason: '残留旧术语的文件：$offenders');
  });
}
