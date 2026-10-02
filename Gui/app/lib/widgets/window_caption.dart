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

/// Resolved visual style for a caption button in a given interaction state.
///
/// v5 replaces the old framed button with a **solid square color block**: there
/// is no border at all — each window key is a single flat fill (one semantic
/// hue per key), hover brightens the fill one step and adds a very weak
/// same-hue glow, and a press darkens it.
@immutable
class WindowCaptionStyle {
  const WindowCaptionStyle({
    required this.background,
    required this.color,
    required this.glowShadows,
  });

  /// Solid block fill; the key's semantic hue, brightened on hover and
  /// darkened while pressed. Never framed.
  final Color background;

  /// Glyph stroke color, drawn on top of the block.
  final Color color;

  /// Same-hue glow layers drawn behind the glyph (empty at rest).
  final List<Shadow> glowShadows;
}

/// Paints a caption glyph as geometric strokes (design-system §3.1: square).
///
/// The symbol is a vector path, not a font glyph, so it stays crisp at 1.5px
/// and never depends on an icon font. v5 draws it ~14px in a dark stroke over
/// the key's solid color block (highest contrast on the bright cyan / violet /
/// magenta fills). Hover glow is same-hue: a cyan block glows cyan, the violet
/// key violet and the magenta key magenta — the fills never cross-colorize.
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
    const half = 7.0;
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
          const offset = Offset(3, -3);
          final back = Rect.fromCenter(
            center: center + offset,
            width: 11,
            height: 11,
          );
          final front = Rect.fromCenter(
            center: center - offset,
            width: 11,
            height: 11,
          );
          return <Path>[Path()..addRect(back), Path()..addRect(front)];
        }
        return <Path>[
          Path()..addRect(Rect.fromCenter(center: center, width: 14, height: 14)),
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
/// v5 drops the framed button for a **solid square color block**, flush to the
/// window's top-right corner: one semantic hue per key (minimize = light cyan,
/// maximize/restore = violet, close = magenta), a ~14px dark stroke glyph, and
/// no border at all. Hover brightens the fill one step and adds a very weak
/// same-hue glow; pressing darkens the fill. Color + glyph + tooltip still
/// carry the design-system §6 triple encoding.
class WindowCaptionButton extends StatefulWidget {
  const WindowCaptionButton({
    super.key,
    required this.kind,
    required this.tooltip,
    this.maximized = false,
    this.onPressed,
  });

  /// Fixed chrome cell width [logical px]; all three keys are equal width.
  static const double width = 44;

  /// Fixed chrome cell height [logical px].
  static const double height = 30;

  /// Hover fill brightening, as a white lerp fraction.
  static const double hoverLift = 0.18;

  /// Pressed fill darkening, as a black lerp fraction.
  static const double pressDrop = 0.22;

  /// Which glyph to draw.
  final WindowCaptionButtonKind kind;

  /// Tooltip text; together with glyph and color this completes the
  /// design-system §6 triple encoding (color + icon + text).
  final String tooltip;

  /// Whether the window is maximized; switches the maximize glyph to restore.
  final bool maximized;

  /// Tap callback, injected so the button can be tested without a window.
  final VoidCallback? onPressed;

  /// The key's solid fill hue at rest, before any hover / press adjustment.
  ///
  /// Sourced from the palette's semantic spectrum: minimize is a lightened
  /// [GuiPalette.valueCyan] (the "浅青" block), maximize [GuiPalette.valuePurple]
  /// and close [GuiPalette.valueMagenta].
  static Color fillFor(GuiPalette palette, WindowCaptionButtonKind kind) {
    switch (kind) {
      case WindowCaptionButtonKind.minimize:
        return Color.lerp(palette.valueCyan, Colors.white, 0.22)!;
      case WindowCaptionButtonKind.maximize:
        return palette.valuePurple;
      case WindowCaptionButtonKind.close:
        return palette.valueMagenta;
    }
  }

  /// Resolves the solid fill, glyph color and glow layers for [kind] in the
  /// given interaction state.
  ///
  /// Centralized (and public) so tests assert the styling directly instead of
  /// relying on pointer hit-testing. There is deliberately **no border**: at
  /// rest the fill is [fillFor]; [hovered] lifts it by [hoverLift] and adds one
  /// weak same-hue glow layer; [pressed] drops it by [pressDrop]. The glyph is
  /// always a dark [GuiPalette.bgVoid] stroke for contrast on the bright block.
  static WindowCaptionStyle styleFor(
    GuiPalette palette,
    WindowCaptionButtonKind kind, {
    required bool hovered,
    bool pressed = false,
  }) {
    final base = fillFor(palette, kind);
    final background = pressed
        ? Color.lerp(base, Colors.black, pressDrop)!
        : (hovered ? Color.lerp(base, Colors.white, hoverLift)! : base);
    final glowShadows = (hovered && !pressed)
        ? GuiGlowStyles.status(base, const TextStyle()).shadows!
        : const <Shadow>[];
    return WindowCaptionStyle(
      background: background,
      color: palette.bgVoid,
      glowShadows: glowShadows,
    );
  }

  @override
  State<WindowCaptionButton> createState() => _WindowCaptionButtonState();
}

class _WindowCaptionButtonState extends State<WindowCaptionButton> {
  bool _hovered = false;
  bool _pressed = false;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final style = WindowCaptionButton.styleFor(
      palette,
      widget.kind,
      hovered: _hovered,
      pressed: _pressed,
    );
    return Tooltip(
      message: widget.tooltip,
      child: MouseRegion(
        cursor: SystemMouseCursors.basic,
        onEnter: (_) => setState(() => _hovered = true),
        onExit: (_) => setState(() => _hovered = false),
        // Press feedback uses a raw Listener: it is independent of the gesture
        // arena, so the fill darkens the instant the pointer goes down instead
        // of waiting on the Tooltip's competing recognizers.
        child: Listener(
          onPointerDown: (_) => setState(() => _pressed = true),
          onPointerUp: (_) => setState(() => _pressed = false),
          onPointerCancel: (_) => setState(() => _pressed = false),
          child: GestureDetector(
            behavior: HitTestBehavior.opaque,
            onTap: widget.onPressed,
            child: SizedBox(
              width: WindowCaptionButton.width,
              height: WindowCaptionButton.height,
              child: Container(
                key: Key('window_caption_surface_${widget.kind.name}'),
                decoration: BoxDecoration(color: style.background),
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

  /// Cluster width [logical px]: three equal-width keys, for reserving the
  /// corner slot in the top bar when the cluster is overlaid flush.
  static const double width = WindowCaptionButton.width * 3;

  /// Cluster height [logical px].
  static const double height = WindowCaptionButton.height;

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
