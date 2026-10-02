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
/// v5 restyles each window key as an **old lattice-lamp indicator**: a
/// desaturated, translucent lamp face ([background]) under a dark grille,
/// ringed by a 1px accent socket ([border]) and seated in a dark vignette
/// ([outlineShadows]). Hover lifts the lamp one step and adds a weak same-hue
/// glow; press dims it.
@immutable
class WindowCaptionStyle {
  const WindowCaptionStyle({
    required this.background,
    required this.color,
    required this.glowShadows,
    required this.border,
    required this.outlineShadows,
  });

  /// Translucent, desaturated lamp face; the key's semantic hue muted and
  /// brightened one step on hover / dimmed while pressed. Never an opaque cap.
  final Color background;

  /// Light glyph stroke drawn on top of the dark grille.
  final Color color;

  /// Same-hue glow layers drawn behind the glyph (empty at rest).
  final List<Shadow> glowShadows;

  /// 1px accent-cyan socket ring around the lamp face.
  final BorderSide border;

  /// Dark outward vignette that seats the lamp in a recessed socket.
  final List<BoxShadow> outlineShadows;
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

/// Paints the lampshade grille laid over a caption lamp face (brief item 2).
///
/// A **bidirectional 1px dark line lattice** at [spacing] px pitch: the "old
/// lattice lampshade" grille vocabulary. It is a grid rather than the VFD bar's
/// vertical tubes, but shares the same dark-line / periodic-cell language.
class WindowCaptionLatticePainter extends CustomPainter {
  const WindowCaptionLatticePainter({required this.color});

  /// Grille cell pitch [px] (brief: 格距 ~4px).
  static const double spacing = 4.0;

  /// Grille line width [px] (brief: 暗色 1px).
  static const double lineWidth = 1.0;

  /// Dark grille line colour, laid over the translucent lamp face.
  final Color color;

  /// Line offsets `0, spacing, 2·spacing, …` across [extent], both axes.
  static List<double> latticeLines(double extent) => <double>[
    for (var x = 0.0; x <= extent; x += spacing) x,
  ];

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = color
      ..strokeWidth = lineWidth;
    for (final x in latticeLines(size.width)) {
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), paint);
    }
    for (final y in latticeLines(size.height)) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), paint);
    }
  }

  @override
  bool shouldRepaint(WindowCaptionLatticePainter oldDelegate) =>
      oldDelegate.color != color;
}

/// A single 44×30 square caption button.
///
/// v5 styles each key as an **old lattice-lamp indicator**, inset from the
/// window's top-right corner: one semantic hue per key (minimize = light cyan,
/// maximize/restore = violet, close = magenta) shown as a desaturated
/// translucent lamp face under a dark grille, ringed by a 1px accent socket and
/// seated in a dark vignette. A ~14px light hue stroke glyph rides on top.
/// Hover lifts the lamp one step and adds a weak same-hue glow; pressing dims
/// it. Color + glyph + tooltip still carry the §6 triple encoding.
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

  /// Lamp-face fill opacity (brief item 1: α ≈ 0.45~0.55). The block reads as a
  /// dim lamp glowing behind a grille, not a solid key cap. Local constant
  /// sourced from the v5 lattice-lamp brief, not a palette token.
  static const double fillAlpha = 0.5;

  /// Lamp-face desaturation toward neutral grey (brief item 1: 色彩向灰调收).
  /// Local constant sourced from the v5 lattice-lamp brief.
  static const double fillDesaturate = 0.4;

  /// Hover fill brightening, as a white lerp fraction.
  static const double hoverLift = 0.18;

  /// Hover also raises the translucent lamp-face alpha one step.
  static const double hoverAlphaLift = 0.12;

  /// Pressed fill darkening, as a black lerp fraction.
  static const double pressDrop = 0.22;

  /// Pressed lamp-face alpha drop.
  static const double pressAlphaDrop = 0.15;

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

  /// The desaturated, translucent lamp face for [kind].
  ///
  /// Brief item 1: the semantic hue ([fillFor]) is pulled toward a neutral grey
  /// of the same level ([fillDesaturate]) and made translucent ([fillAlpha]) so
  /// the key reads as a dim indicator lamp, not an opaque cap.
  static Color lampFaceFor(GuiPalette palette, WindowCaptionButtonKind kind) {
    final hue = fillFor(palette, kind);
    final greyLevel = (hue.r + hue.g + hue.b) / 3;
    final grey = Color.from(
      alpha: 1,
      red: greyLevel,
      green: greyLevel,
      blue: greyLevel,
    );
    return Color.lerp(hue, grey, fillDesaturate)!.withValues(alpha: fillAlpha);
  }

  /// The light glyph stroke for [kind].
  ///
  /// Brief item 6: a light stroke reads on the dark grille, so the key hue is
  /// lightened while staying per-key distinct (triple encoding).
  static Color glyphFor(GuiPalette palette, WindowCaptionButtonKind kind) =>
      Color.lerp(fillFor(palette, kind), Colors.white, 0.72)!;

  /// Resolves the lamp face, glyph color, glow, socket ring and vignette for
  /// [kind] in the given interaction state.
  ///
  /// Centralized (and public) so tests assert the styling directly instead of
  /// relying on pointer hit-testing. At rest the face is [lampFaceFor];
  /// [hovered] lifts it by [hoverLift] and raises its alpha by [hoverAlphaLift]
  /// plus one weak same-hue glow; [pressed] darkens it by [pressDrop] and drops
  /// its alpha by [pressAlphaDrop]. The glyph is the light [glyphFor] stroke,
  /// the socket ring is the 1px accent cyan and the vignette is a dark outward
  /// box shadow (brief item 3/4).
  static WindowCaptionStyle styleFor(
    GuiPalette palette,
    WindowCaptionButtonKind kind, {
    required bool hovered,
    bool pressed = false,
  }) {
    final base = lampFaceFor(palette, kind);
    final Color background;
    if (pressed) {
      background = Color.lerp(base, Colors.black, pressDrop)!.withValues(
        alpha: (base.a - pressAlphaDrop).clamp(0.0, 1.0),
      );
    } else if (hovered) {
      background = Color.lerp(base, Colors.white, hoverLift)!.withValues(
        alpha: (base.a + hoverAlphaLift).clamp(0.0, 1.0),
      );
    } else {
      background = base;
    }
    final hue = fillFor(palette, kind);
    final glowShadows = (hovered && !pressed)
        ? GuiGlowStyles.status(hue, const TextStyle()).shadows!
        : const <Shadow>[];
    return WindowCaptionStyle(
      background: background,
      color: glyphFor(palette, kind),
      glowShadows: glowShadows,
      border: BorderSide(color: palette.accent, width: 1),
      outlineShadows: const <BoxShadow>[
        BoxShadow(color: Color(0x8C000000), blurRadius: 8, spreadRadius: 1),
      ],
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
                decoration: BoxDecoration(
                  color: style.background,
                  border: Border.fromBorderSide(style.border),
                  boxShadow: style.outlineShadows,
                ),
                child: Stack(
                  fit: StackFit.expand,
                  children: <Widget>[
                    // 格栅罩: dark 1px lattice over the translucent lamp face.
                    CustomPaint(
                      painter: WindowCaptionLatticePainter(
                        color: palette.bgVoid.withValues(alpha: 0.5),
                      ),
                    ),
                    CustomPaint(
                      painter: WindowCaptionGlyphPainter(
                        kind: widget.kind,
                        color: style.color,
                        glowShadows: style.glowShadows,
                        maximized: widget.maximized,
                      ),
                    ),
                  ],
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
  /// corner slot in the top bar.
  static const double width = WindowCaptionButton.width * 3;

  /// Cluster height [logical px].
  static const double height = WindowCaptionButton.height;

  /// Right inset [logical px] applied when the cluster is overlaid in the top
  /// bar (brief item 5): the keys pull ~10px off the window corner so the lamp
  /// sockets and their vignette have breathing room from the cyan window frame.
  static const double rightInset = 10;

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
