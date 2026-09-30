import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';

/// Which caption affordance a [WindowCaptionButton] draws.
enum WindowCaptionButtonKind {
  /// Horizontal bar: minimize.
  minimize,

  /// Square outline: maximize (a double frame once maximized).
  maximize,

  /// Two crossing diagonals: close.
  close,
}

/// Resolved visual style for a caption button in a given hover state.
@immutable
class WindowCaptionStyle {
  const WindowCaptionStyle({
    required this.background,
    required this.color,
    required this.glowShadows,
  });

  /// Button fill; transparent at rest.
  final Color background;

  /// Glyph stroke color.
  final Color color;

  /// Same-hue glow layers drawn behind the glyph (empty at rest).
  final List<Shadow> glowShadows;
}

/// Paints a caption glyph as geometric strokes (design-system §3.1: square).
///
/// The symbol is a vector path, not a font glyph, so it stays crisp at 1.5px
/// and never depends on an icon font. Hover glow reuses the palette's
/// same-hue glow tiers (design-system §2.4), so a red close button glows red
/// and every other button glows cyan — status hues never cross.
class WindowCaptionGlyphPainter extends CustomPainter {
  const WindowCaptionGlyphPainter({
    required this.kind,
    required this.color,
    required this.glowShadows,
    this.maximized = false,
    this.strokeWidth = 1.5,
  });

  /// Which glyph to draw.
  final WindowCaptionButtonKind kind;

  /// Glyph stroke color.
  final Color color;

  /// Same-hue glow layers drawn behind the stroke.
  final List<Shadow> glowShadows;

  /// When true the [WindowCaptionButtonKind.maximize] glyph becomes the
  /// two-frame "restore" symbol.
  final bool maximized;

  /// Glyph stroke width.
  final double strokeWidth;

  /// Builds the glyph geometry inside a [size] canvas.
  ///
  /// Exposed so tests can assert the maximize/restore frame count without
  /// rasterizing. The listing returns one path per frame (or one per diagonal
  /// for close).
  static List<Path> buildPaths(
    WindowCaptionButtonKind kind,
    Size size, {
    bool maximized = false,
  }) {
    final center = Offset(size.width / 2, size.height / 2);
    const half = 5.0;
    switch (kind) {
      case WindowCaptionButtonKind.minimize:
        return <Path>[
          Path()
            ..moveTo(center.dx - half, center.dy)
            ..lineTo(center.dx + half, center.dy),
        ];
      case WindowCaptionButtonKind.close:
        return <Path>[
          Path()
            ..moveTo(center.dx - half, center.dy - half)
            ..lineTo(center.dx + half, center.dy + half),
          Path()
            ..moveTo(center.dx + half, center.dy - half)
            ..lineTo(center.dx - half, center.dy + half),
        ];
      case WindowCaptionButtonKind.maximize:
        if (maximized) {
          const offset = Offset(2.5, -2.5);
          final back = Rect.fromCenter(
            center: center + offset,
            width: 8,
            height: 8,
          );
          final front = Rect.fromCenter(
            center: center - offset,
            width: 8,
            height: 8,
          );
          return <Path>[Path()..addRect(back), Path()..addRect(front)];
        }
        return <Path>[
          Path()..addRect(Rect.fromCenter(center: center, width: 10, height: 10)),
        ];
    }
  }

  @override
  void paint(Canvas canvas, Size size) {
    final paths = buildPaths(kind, size, maximized: maximized);
    for (final shadow in glowShadows) {
      final glowPaint = Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = strokeWidth
        ..strokeCap = StrokeCap.square
        ..color = shadow.color
        ..maskFilter = shadow.blurRadius > 0
            ? ui.MaskFilter.blur(ui.BlurStyle.normal, shadow.blurRadius)
            : null;
      for (final path in paths) {
        canvas.drawPath(path, glowPaint);
      }
    }
    final strokePaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = strokeWidth
      ..strokeCap = StrokeCap.square
      ..strokeJoin = StrokeJoin.miter
      ..color = color;
    for (final path in paths) {
      canvas.drawPath(path, strokePaint);
    }
  }

  @override
  bool shouldRepaint(WindowCaptionGlyphPainter oldDelegate) =>
      oldDelegate.kind != kind ||
      oldDelegate.color != color ||
      oldDelegate.maximized != maximized ||
      oldDelegate.strokeWidth != strokeWidth ||
      !listEquals(oldDelegate.glowShadows, glowShadows);
}

/// A single 44×30 square caption button.
///
/// Dimension and radius come from the design-system structure tokens: square
/// corners (§3.1) and a fixed chrome cell. Hover matches the rest of the chrome
/// (`bgHover`); close is the one deliberate exception — its hover fill is the
/// fault color with a fault-colored glow and a near-white glyph, so the
/// destructive affordance reads unambiguously and its glow never turns cyan.
class WindowCaptionButton extends StatefulWidget {
  const WindowCaptionButton({
    super.key,
    required this.kind,
    required this.tooltip,
    this.maximized = false,
    this.onPressed,
  });

  /// Which glyph to draw.
  final WindowCaptionButtonKind kind;

  /// Tooltip text; together with glyph and hover color this completes the
  /// design-system §6 triple encoding (color + icon + text).
  final String tooltip;

  /// Whether the window is maximized; switches the maximize glyph to restore.
  final bool maximized;

  /// Tap callback, injected so the button can be tested without a window.
  final VoidCallback? onPressed;

  /// Resolves the fill, glyph color and glow layers for [kind] and [hovered].
  ///
  /// Centralized (and public) so tests assert the hover styling directly
  /// instead of relying on pointer hit-testing.
  static WindowCaptionStyle styleFor(
    GuiPalette palette,
    WindowCaptionButtonKind kind, {
    required bool hovered,
  }) {
    final isClose = kind == WindowCaptionButtonKind.close;
    final background = hovered
        ? (isClose ? palette.stFault : palette.bgHover)
        : Colors.transparent;
    final color = (hovered && isClose) ? palette.fgStrong : palette.fg;
    final List<Shadow> glowShadows;
    if (!hovered) {
      glowShadows = const <Shadow>[];
    } else if (isClose) {
      glowShadows =
          GuiGlowStyles.status(palette.stFault, const TextStyle()).shadows!;
    } else {
      glowShadows = GuiGlowStyles.softWith(palette, const TextStyle()).shadows!;
    }
    return WindowCaptionStyle(
      background: background,
      color: color,
      glowShadows: glowShadows,
    );
  }

  @override
  State<WindowCaptionButton> createState() => _WindowCaptionButtonState();
}

class _WindowCaptionButtonState extends State<WindowCaptionButton> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final style = WindowCaptionButton.styleFor(
      palette,
      widget.kind,
      hovered: _hovered,
    );
    return Tooltip(
      message: widget.tooltip,
      child: MouseRegion(
        cursor: SystemMouseCursors.basic,
        onEnter: (_) => setState(() => _hovered = true),
        onExit: (_) => setState(() => _hovered = false),
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTap: widget.onPressed,
          child: SizedBox(
            width: 44,
            height: 30,
            child: ColoredBox(
              color: style.background,
              child: CustomPaint(
                painter: WindowCaptionGlyphPainter(
                  kind: widget.kind,
                  color: style.color,
                  glowShadows: style.glowShadows,
                  maximized: widget.maximized,
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

/// The minimize / maximize-restore / close cluster at the far right of the top
/// bar. Callbacks are injected so the whole cluster is widget-testable.
class WindowCaptionBar extends StatelessWidget {
  const WindowCaptionBar({
    super.key,
    this.maximized = false,
    this.onMinimize,
    this.onToggleMaximize,
    this.onClose,
  });

  /// Whether the window is currently maximized.
  final bool maximized;

  /// Minimize callback.
  final VoidCallback? onMinimize;

  /// Maximize/restore toggle callback.
  final VoidCallback? onToggleMaximize;

  /// Close callback.
  final VoidCallback? onClose;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        WindowCaptionButton(
          key: const Key('window_caption_minimize'),
          kind: WindowCaptionButtonKind.minimize,
          tooltip: '最小化',
          onPressed: onMinimize,
        ),
        WindowCaptionButton(
          key: const Key('window_caption_maximize'),
          kind: WindowCaptionButtonKind.maximize,
          tooltip: '最大化/还原',
          maximized: maximized,
          onPressed: onToggleMaximize,
        ),
        WindowCaptionButton(
          key: const Key('window_caption_close'),
          kind: WindowCaptionButtonKind.close,
          tooltip: '关闭',
          onPressed: onClose,
        ),
      ],
    );
  }
}
