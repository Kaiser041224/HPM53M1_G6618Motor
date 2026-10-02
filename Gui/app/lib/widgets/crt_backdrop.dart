/// 内容区 CRT 背景：静止扫描线 + ~12Hz 动态噪点。
///
/// 挂在页面内容之下的纯装饰层（TNO 硅晶之梦 v5）：一层静止的水平扫描线
/// （每 ~3px 一条 1px 暗线，α≈0.06）加一层稀疏、极低亮度的随机噪点
/// （亮点 / 暗点，α≈0.03~0.06）。噪点由定时器按 ~12Hz 重新生成，因此是
/// **动态变化**而非纯色；所有纹理都在内容之下，绝不上叠到文字。
///
/// 性能：噪点重绘被 [RepaintBoundary] 隔离，且只通过 `repaint` 监听器触发
/// `CustomPainter` 重绘，不重建控件树。定时器**常驻不停表**（背景挂在页面
/// 栈之外，切页时保持连续），`dispose` 中释放。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';

/// 噪点重新生成的间隔（约 12Hz）。
const Duration kCrtNoiseInterval = Duration(milliseconds: 83);

/// 内容区 CRT 背景层。
///
/// 放在 `Stack` 的最底层、页面内容之下。无子节点，自身铺满约束。
class CrtBackdrop extends StatefulWidget {
  const CrtBackdrop({super.key, this.noiseInterval = kCrtNoiseInterval});

  /// 噪点重绘间隔（测试可缩短以加速）。
  final Duration noiseInterval;

  @override
  State<CrtBackdrop> createState() => _CrtBackdropState();
}

class _CrtBackdropState extends State<CrtBackdrop> {
  /// 噪点 seed —— 递增即触发 `CustomPainter` 重绘，不引起 setState / 重建。
  final ValueNotifier<int> _noiseSeed = ValueNotifier<int>(0);

  Timer? _noiseTimer;

  @override
  void initState() {
    super.initState();
    // 常驻 ticker：背景在页面栈之外，切页不停表（离屏省电方案未采用）。
    _noiseTimer = Timer.periodic(
      widget.noiseInterval,
      (_) => _noiseSeed.value++,
    );
  }

  @override
  void dispose() {
    _noiseTimer?.cancel();
    _noiseSeed.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    return IgnorePointer(
      key: const Key('crt_backdrop'),
      child: RepaintBoundary(
        child: CustomPaint(
          key: const Key('crt_canvas'),
          painter: CrtScanlinePainter(
            color: palette.bgVoid.withValues(alpha: CrtScanlinePainter.alpha),
          ),
          foregroundPainter: CrtNoisePainter(
            seed: _noiseSeed,
            bright: palette.fgStrong,
            dark: palette.bgVoid,
          ),
          isComplex: true,
          willChange: true,
          size: Size.infinite,
        ),
      ),
    );
  }
}

/// 静止的水平扫描线：每 [spacing] px 一条 1px 暗线。
class CrtScanlinePainter extends CustomPainter {
  const CrtScanlinePainter({required this.color, this.spacing = 3});

  /// 扫描线颜色（调用方已含 alpha）。
  final Color color;

  /// 线距 [logical px]。
  final double spacing;

  /// 扫描线暗度 α（设计范围 0.05~0.08）。
  static const double alpha = 0.06;

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = color
      ..strokeWidth = 1;
    for (double y = 0; y < size.height; y += spacing) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), paint);
    }
  }

  @override
  bool shouldRepaint(covariant CrtScanlinePainter oldDelegate) =>
      oldDelegate.color != color || oldDelegate.spacing != spacing;
}

/// 稀疏、极低亮度的随机噪点；[seed] 每次递增即重绘一帧新的噪点。
class CrtNoisePainter extends CustomPainter {
  CrtNoisePainter({
    required this.seed,
    required this.bright,
    required this.dark,
    this.density = 0.0004,
  }) : super(repaint: seed);

  /// 噪点代数来源；`generation` 变化即表示有一帧新的噪点。
  final ValueListenable<int> seed;

  /// 亮点基色。
  final Color bright;

  /// 暗点基色。
  final Color dark;

  /// 每平方逻辑像素的噪点数（稀疏）。
  final double density;

  /// 当前噪点代数（测试观测用）。
  int get generation => seed.value;

  @override
  void paint(Canvas canvas, Size size) {
    final rand = math.Random(seed.value);
    final count = (size.width * size.height * density).round().clamp(0, 600);
    final paint = Paint();
    for (var i = 0; i < count; i++) {
      final x = rand.nextDouble() * size.width;
      final y = rand.nextDouble() * size.height;
      final alpha = 0.03 + rand.nextDouble() * 0.03;
      paint.color = (rand.nextBool() ? bright : dark).withValues(alpha: alpha);
      canvas.drawRect(Rect.fromLTWH(x, y, 1, 1), paint);
    }
  }

  @override
  bool shouldRepaint(covariant CrtNoisePainter oldDelegate) =>
      oldDelegate.generation != generation ||
      oldDelegate.bright != bright ||
      oldDelegate.dark != dark;
}
