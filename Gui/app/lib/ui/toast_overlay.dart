/// 全局悬浮提示（Toast Overlay）。
///
/// 取代 `ScaffoldMessenger.showSnackBar`：**底部水平居中**贴底轻浮，
/// 悬于状态栏（[BottomBar]）顶缘上方 [kGuiToastBottomGap] px。双层青色
/// 锐利矩形从底缘向上滑入定格，约 2.2s 后淡出。挂到最近的 [Overlay]
/// （[SinglePage] 层），全站统一提示语汇。
///
/// 语气用三重编码：图标 + 色带 + 颜色（info 青 / success 翠绿 / warn 橙 /
/// error 红），即使配色被忽略，图标仍能读出语气。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';

import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Toast 语气（决定图标与强调色）。
enum ToastTone {
  /// 中性信息。
  info,

  /// 成功。
  success,

  /// 警告。
  warn,

  /// 失败 / 错误。
  error,
}

/// 整场寿命 [ms]：入场 + 定格 + 退场（一次性动画，避免残留 Timer）。
const int _kToastTotalMs = 2340;
const int _kToastEntranceMs = 200;
const int _kToastExitMs = 160;
const double _kToastEntranceFraction = _kToastEntranceMs / _kToastTotalMs;
const double _kToastExitFraction = _kToastExitMs / _kToastTotalMs;

/// 入场滑入距离 [logical px]（自下方底缘）。
const double _kToastRise = 24;

/// 底部状态栏高度 [logical px]（[BottomBar]：8px 上下内边距 + 内容行）。
const double kGuiToastStatusBarHeight = 40;

/// toast 底缘与状态栏顶缘之间的间隙 [logical px]。
const double kGuiToastBottomGap = 12;

/// toast 底缘距窗口底缘的距离 [logical px] = 状态栏高度 + 间隙。
const double kGuiToastBottomInset =
    kGuiToastStatusBarHeight + kGuiToastBottomGap;

/// 在 [context] 最近的 Overlay 上弹出一个提示。
void showGuiToast(
  BuildContext context,
  String message, {
  ToastTone tone = ToastTone.info,
}) {
  final overlay = Overlay.maybeOf(context);
  if (overlay == null) return;
  showGuiToastIn(overlay, message, tone: tone);
}

/// 在已捕获的 [overlay] 上弹出提示 —— 供 await 之后再提示的调用点使用，
/// 避免跨越异步间隙引用已失效的 [BuildContext]。
void showGuiToastIn(
  OverlayState overlay,
  String message, {
  ToastTone tone = ToastTone.info,
}) {
  late final OverlayEntry entry;
  var removed = false;
  void remove() {
    if (removed) return;
    removed = true;
    if (entry.mounted) entry.remove();
  }

  entry = OverlayEntry(
    builder: (_) =>
        _GuiToast(message: message, tone: tone, onDismissed: remove),
  );
  overlay.insert(entry);
}

/// 单条提示：一次性 AnimationController 驱动「弹出 → 定格 → 消退」，完成即移除。
class _GuiToast extends StatefulWidget {
  const _GuiToast({
    required this.message,
    required this.tone,
    required this.onDismissed,
  });

  final String message;
  final ToastTone tone;
  final VoidCallback onDismissed;

  @override
  State<_GuiToast> createState() => _GuiToastState();
}

class _GuiToastState extends State<_GuiToast>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller;

  @override
  void initState() {
    super.initState();
    _controller =
        AnimationController(
          vsync: this,
          duration: const Duration(milliseconds: _kToastTotalMs),
        )..addStatusListener((status) {
          if (status == AnimationStatus.completed) widget.onDismissed();
        });
    _controller.forward();
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  /// 入场进度：从下方底缘向上滑入（~200ms easeOut，无过冲）。
  double get _entrance {
    if (_controller.value >= _kToastEntranceFraction) return 1;
    return Curves.easeOut.transform(
      _controller.value / _kToastEntranceFraction,
    );
  }

  /// 退场不透明度：定格段恒为 1，末段线性淡出。
  double get _opacity {
    final value = _controller.value;
    if (value <= 1 - _kToastExitFraction) return 1;
    return ((1 - value) / _kToastExitFraction).clamp(0.0, 1.0);
  }

  static Color _toneColor(GuiPalette palette, ToastTone tone) => switch (tone) {
    ToastTone.info => palette.accent,
    ToastTone.success => palette.stNormal,
    ToastTone.warn => palette.stWarn,
    ToastTone.error => palette.stFault,
  };

  static IconData _toneIcon(ToastTone tone) => switch (tone) {
    ToastTone.info => Icons.info_outline,
    ToastTone.success => Icons.check_circle_outline,
    ToastTone.warn => Icons.warning_amber_outlined,
    ToastTone.error => Icons.error_outline,
  };

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final toneColor = _toneColor(palette, widget.tone);

    return AnimatedBuilder(
      animation: _controller,
      builder: (context, _) {
        final dy = (1 - _entrance) * _kToastRise;
        return Positioned.fill(
          child: IgnorePointer(
            child: Align(
              key: const Key('gui_toast'),
              alignment: Alignment.bottomCenter,
              child: Padding(
                padding: const EdgeInsets.only(bottom: kGuiToastBottomInset),
                child: Transform.translate(
                  offset: Offset(0, dy),
                  child: Opacity(
                    opacity: _opacity,
                    child: ConstrainedBox(
                      constraints: const BoxConstraints(maxWidth: 460),
                      child: Container(
                        key: const Key('gui_toast_frame'),
                        decoration: BoxDecoration(
                          color: palette.bgBase,
                          border: Border.all(color: palette.accent, width: 1.5),
                        ),
                        padding: const EdgeInsets.all(1.5),
                        child: Container(
                          key: const Key('gui_toast_inner'),
                          decoration: BoxDecoration(
                            border: Border.all(
                              color: palette.lineDim,
                              width: palette.lineWidth,
                            ),
                          ),
                          padding: const EdgeInsets.symmetric(
                            horizontal: 14,
                            vertical: 10,
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: <Widget>[
                              // 语气色带：颜色编码（不单独承载，与图标并存）。
                              Container(width: 4, height: 22, color: toneColor),
                              const SizedBox(width: 10),
                              Icon(
                                _toneIcon(widget.tone),
                                key: const Key('gui_toast_icon'),
                                size: 18,
                                color: toneColor,
                              ),
                              const SizedBox(width: 10),
                              Flexible(
                                child: Text(
                                  widget.message,
                                  key: const Key('gui_toast_text'),
                                  maxLines: 3,
                                  overflow: TextOverflow.ellipsis,
                                  style: GuiGlowStyles.weak(
                                    scale.body.copyWith(
                                      fontSize: 13,
                                      color: palette.fgStrong,
                                    ),
                                  ),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}
