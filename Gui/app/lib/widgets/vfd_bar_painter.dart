import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';

/// Maps a physical dimension to its v5 accent tint (design-system §2.6.3).
///
/// The accent is derived from the **unit**, never from a parameter-name
/// whitelist, so a new quantity automatically inherits the correct colour:
/// `V` / `°` → cyan, `A` / `RPM` → green, `W` / `%` → magenta, `°C` → orange,
/// `Hz` → violet, and a missing unit falls back to neutral white-cyan.
Color tintForUnit(GuiPalette palette, String unit) {
  switch (unit.trim().toUpperCase()) {
    case 'V':
    case '°':
      return palette.valueCyan;
    case 'A':
    case 'RPM':
      return palette.valueGreen;
    case 'W':
    case '%':
      return palette.valueMagenta;
    case '°C':
    case '℃':
      return palette.valueOrange;
    case 'HZ':
      return palette.valuePurple;
    default:
      return palette.fgStrong;
  }
}

/// VFD 竖排发光管条 painter (design-system §2.6.5).
///
/// Paints a sealed, frosted VFD package: a slotted trough whose dark
/// unlit-tube "ghosts" run the full width, a bank of vertical 1.5px lit tubes
/// filling the left `ratio` fraction, spill light on the unlit tubes just past
/// the lit edge, three large-radius frosted glow layers, 50%/85% threshold
/// ticks and an optional magenta peak-hold line.
///
/// All dimension-driven colour comes in through [tint] (from [tintForUnit]);
/// the sealed-package constants (slot, glass, ghost, edge) are painter-local
/// values copied verbatim from `previews/gui-style-sample.html` /
/// `tokens.css` `.gui-vfd`, documented below.
class VfdBarPainter extends CustomPainter {
  const VfdBarPainter({
    required this.ratio,
    required this.tint,
    this.peak,
    this.warnBand = true,
  });

  /// Filled fraction, nominally `[0, 1]`; values above one saturate the lit
  /// width at full scale while the caller signals the over-limit with its tint.
  final double ratio;

  /// Accent tube colour, derived from the dimension.
  final Color tint;

  /// Peak-hold marker position as a fraction, or null to draw no marker.
  final double? peak;

  /// Whether to draw the 50% / 85% threshold ticks.
  final bool warnBand;

  // ---- Tube geometry (§2.6.5): 4px period = seam .5 + feather 1 + core 1.5 + feather 1.
  /// One tube + seam period, px.
  static const double tubePeriod = 4.0;

  /// Dark seam at the start of a period, px.
  static const double seamWidth = 0.5;

  /// Feather (falloff) on each side of the core, px.
  static const double featherWidth = 1.0;

  /// Bright core width, px.
  static const double coreWidth = 1.5;

  /// Lit-core opacity (§2.6.5: bright core @85%).
  static const double coreAlpha = 0.85;

  /// Lit-feather opacity (§2.6.5: feather @16%).
  static const double featherAlpha = 0.16;

  /// Unlit ghost core opacity (§2.6.5: base ≈ 2.8%).
  static const double ghostCoreAlpha = 0.028;

  /// Unlit ghost feather opacity.
  static const double ghostFeatherAlpha = 0.016;

  // ---- Spill (§2.6.5): unlit tubes brighten within 30px past the lit edge.
  /// Spill reach past the lit edge, px.
  static const double spillWidth = 30.0;

  /// Spill core opacity at the lit edge (§2.6.5: α .05 → .12).
  static const double spillCoreAlpha = 0.12;

  /// Spill feather opacity at the lit edge.
  static const double spillFeatherAlpha = 0.05;

  // ---- Frosted glow (§2.6.5): three large-radius layers.
  /// Blur radii of the three frosted glow layers, px.
  static const List<double> glowRadii = <double>[4, 10, 22];

  /// Opacities of the three frosted glow layers (α .5 / .3 / .18).
  static const List<double> glowAlphas = <double>[0.5, 0.3, 0.18];

  // ---- Threshold ticks (§2.6.5): 50% / 85% cyan @ α .3.
  /// Threshold mark positions.
  static const List<double> bandMarks = <double>[0.5, 0.85];

  /// Threshold tick opacity (α .3).
  static const double bandAlpha = 0.3;

  // ---- Peak line (§2.6.5): magenta 2px + double glow.
  /// Peak marker width, px.
  static const double peakWidth = 2.0;

  /// Blur radii of the peak double glow, px.
  static const List<double> peakGlowRadii = <double>[16, 6];

  /// Opacities of the peak double glow.
  static const List<double> peakGlowAlphas = <double>[0.35, 0.7];

  /// Default bar height, px (§2.6.5: 22px).
  static const double barHeight = 22.0;

  // ---- Sealed-package constants (source: previews/gui-style-sample.html /
  //      tokens.css `.gui-vfd`; not palette tokens, kept painter-local).
  /// Trough base colour (`#050B0E`).
  static const Color slotColor = Color(0xFF050B0E);

  /// 1px dark outer edge colour (`#0C1A20`).
  static const Color edgeColor = Color(0xFF0C1A20);

  /// Neutral grey-cyan of the unlit ghost tubes (`rgba(140,190,200,·)`).
  static const Color ghostColor = Color(0xFF8CBEC8);

  /// Lit fill width floored to a whole number of tubes to avoid a half tube.
  static double fillWidth(double width, double ratio) {
    final target = (width * ratio).clamp(0.0, width);
    return (target / tubePeriod).floor() * tubePeriod;
  }

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width;
    final h = size.height;
    if (w <= 0 || h <= 0) return;
    final rect = Offset.zero & size;

    // 座落阴影: 1px black just under the trough (box-shadow `0 1px 0`).
    canvas.drawRect(
      Rect.fromLTWH(0, h, w, 1),
      Paint()..color = const Color(0x99000000),
    );

    canvas.save();
    canvas.clipRect(rect);

    // 槽底 + 未点亮管芯幽灵.
    canvas.drawRect(rect, Paint()..color = slotColor);
    _tubes(canvas, size, 0, w, ghostColor, ghostCoreAlpha, ghostFeatherAlpha);

    final fw = fillWidth(w, ratio).clamp(0.0, w);
    if (fw > 0) {
      // 雾面三层光晕: behind the lit tubes, sealed by the trough clip.
      final fillRect = Rect.fromLTWH(0, 0, fw, h);
      for (var i = 0; i < glowRadii.length; i++) {
        canvas.drawRect(
          fillRect,
          Paint()
            ..color = tint.withValues(alpha: glowAlphas[i])
            ..maskFilter = MaskFilter.blur(BlurStyle.normal, glowRadii[i]),
        );
      }
      // 点亮管.
      _litTubes(canvas, size, 0, fw);
      // spill: 亮缘外 30px 内暗管随距离渐亮.
      _spill(canvas, size, fw);
    }

    // 门限刻度 50% / 85%.
    if (warnBand) {
      final bandPaint = Paint()
        ..color = tintForBand.withValues(alpha: bandAlpha);
      for (final mark in bandMarks) {
        canvas.drawRect(
          Rect.fromLTWH(mark * w, 0, 1, h),
          bandPaint,
        );
      }
    }

    // 峰值线: 洋红 2px + 双层辉光.
    final peakValue = peak;
    if (peakValue != null) {
      _peak(canvas, size, peakValue);
    }

    // 凹槽: 上暗内影 + 槽底反光.
    _slotDepth(canvas, size);

    // 玻璃罩 (高光压制).
    _glass(canvas, size);

    canvas.restore();

    // 1px 暗色外缘.
    canvas.drawRect(
      Rect.fromLTWH(0.5, 0.5, w - 1, h - 1),
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = edgeColor,
    );
  }

  /// Threshold ticks use the cyan chrome token, independent of the tube tint.
  Color get tintForBand => const GuiPalette().lineHi;

  void _tubes(
    Canvas canvas,
    Size size,
    double x0,
    double x1,
    Color base,
    double coreA,
    double featherA,
  ) {
    final core = Paint()..color = base.withValues(alpha: coreA);
    final feather = Paint()..color = base.withValues(alpha: featherA);
    for (var x = x0; x + tubePeriod <= x1 + 0.001; x += tubePeriod) {
      canvas.drawRect(
        Rect.fromLTWH(x + seamWidth, 0, featherWidth, size.height),
        feather,
      );
      canvas.drawRect(
        Rect.fromLTWH(x + seamWidth + featherWidth, 0, coreWidth, size.height),
        core,
      );
      canvas.drawRect(
        Rect.fromLTWH(
          x + seamWidth + featherWidth + coreWidth,
          0,
          featherWidth,
          size.height,
        ),
        feather,
      );
    }
  }

  void _litTubes(Canvas canvas, Size size, double x0, double x1) {
    // Bright core is a whitened tint; the feather keeps the pure tint.
    final core = Color.lerp(tint, Colors.white, 0.55)!;
    _tubes(canvas, size, x0, x1, tint, coreAlpha, featherAlpha);
    final corePaint = Paint()..color = core.withValues(alpha: coreAlpha);
    for (var x = x0; x + tubePeriod <= x1 + 0.001; x += tubePeriod) {
      canvas.drawRect(
        Rect.fromLTWH(x + seamWidth + featherWidth, 0, coreWidth, size.height),
        corePaint,
      );
    }
  }

  void _spill(Canvas canvas, Size size, double from) {
    final end = (from + spillWidth).clamp(0.0, size.width);
    final bright = Color.lerp(tint, Colors.white, 0.55)!;
    for (var x = from; x + tubePeriod <= end + 0.001; x += tubePeriod) {
      final mask = (1 - (x - from) / spillWidth).clamp(0.0, 1.0);
      if (mask <= 0) continue;
      final corePaint = Paint()
        ..color = bright.withValues(alpha: spillCoreAlpha * mask);
      final featherPaint = Paint()
        ..color = tint.withValues(alpha: spillFeatherAlpha * mask);
      canvas.drawRect(
        Rect.fromLTWH(x + seamWidth, 0, featherWidth, size.height),
        featherPaint,
      );
      canvas.drawRect(
        Rect.fromLTWH(x + seamWidth + featherWidth, 0, coreWidth, size.height),
        corePaint,
      );
      canvas.drawRect(
        Rect.fromLTWH(
          x + seamWidth + featherWidth + coreWidth,
          0,
          featherWidth,
          size.height,
        ),
        featherPaint,
      );
    }
    // 溢光: a soft same-hue bloom just past the lit edge.
    if (end > from) {
      canvas.drawRect(
        Rect.fromLTWH(from, 0, end - from, size.height),
        Paint()
          ..color = tint.withValues(alpha: 0.10)
          ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 8),
      );
    }
  }

  void _peak(Canvas canvas, Size size, double value) {
    final magenta = const GuiPalette().valueMagenta;
    final left = (value * size.width - peakWidth / 2).clamp(
      0.0,
      (size.width - peakWidth).clamp(0.0, size.width),
    );
    final rect = Rect.fromLTWH(left, 1, peakWidth, size.height - 2);
    for (var i = 0; i < peakGlowRadii.length; i++) {
      canvas.drawRect(
        rect,
        Paint()
          ..color = magenta.withValues(alpha: peakGlowAlphas[i])
          ..maskFilter = MaskFilter.blur(BlurStyle.normal, peakGlowRadii[i]),
      );
    }
    canvas.drawRect(rect, Paint()..color = magenta);
  }

  void _slotDepth(Canvas canvas, Size size) {
    final w = size.width;
    final h = size.height;
    // inset 0 2px 4px rgba(0,0,0,.65): top inner shadow.
    final top = Rect.fromLTWH(0, 0, w, 4);
    canvas.drawRect(
      top,
      Paint()
        ..shader = const LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: <Color>[Color(0xA6000000), Color(0x00000000)],
        ).createShader(top),
    );
    // inset 0 -1px 2px rgba(120,180,190,.08): trough-floor reflection.
    final bottom = Rect.fromLTWH(0, h - 2, w, 2);
    canvas.drawRect(
      bottom,
      Paint()
        ..shader = const LinearGradient(
          begin: Alignment.bottomCenter,
          end: Alignment.topCenter,
          colors: <Color>[Color(0x1478B4BE), Color(0x0078B4BE)],
        ).createShader(bottom),
    );
  }

  void _glass(Canvas canvas, Size size) {
    final rect = Offset.zero & size;
    // vertical: top light .045 → .012 @30% → bottom dark .07 @88%.
    canvas.drawRect(
      rect,
      Paint()
        ..shader = const LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          stops: <double>[0, 0.3, 0.88, 1],
          colors: <Color>[
            Color(0x0CFFFFFF),
            Color(0x03FFFFFF),
            Color(0x12000000),
            Color(0x12000000),
          ],
        ).createShader(rect),
    );
    // horizontal: edge lights .025 at the 12% shoulders.
    canvas.drawRect(
      rect,
      Paint()
        ..shader = const LinearGradient(
          begin: Alignment.centerLeft,
          end: Alignment.centerRight,
          stops: <double>[0, 0.12, 0.88, 1],
          colors: <Color>[
            Color(0x06FFFFFF),
            Color(0x00FFFFFF),
            Color(0x00FFFFFF),
            Color(0x06FFFFFF),
          ],
        ).createShader(rect),
    );
  }

  @override
  bool shouldRepaint(covariant VfdBarPainter oldDelegate) =>
      oldDelegate.ratio != ratio ||
      oldDelegate.peak != peak ||
      oldDelegate.tint != tint ||
      oldDelegate.warnBand != warnBand;
}

/// Thin wrapper: resolves the tinted VFD bar against the ambient layout.
///
/// [ratio] drives the lit fraction, [peak] the optional magenta peak-hold
/// marker, [tint] the tube/spill dimension colour and [warnBand] the 50%/85%
/// threshold ticks. A [ratio] above one is allowed through to the painter,
/// which saturates the lit width at full scale; the over-limit tint is the
/// caller's responsibility (see `HeatLoadBar`).
class VfdBar extends StatelessWidget {
  const VfdBar({
    super.key,
    required this.ratio,
    required this.tint,
    this.peak,
    this.warnBand = true,
    this.height = VfdBarPainter.barHeight,
  });

  /// Filled fraction. Floored at 0; values above one saturate at full scale
  /// while the caller swaps in the over-limit tint.
  final double ratio;

  /// Accent tube colour (see [tintForUnit]).
  final Color tint;

  /// Peak-hold marker fraction, or null.
  final double? peak;

  /// Whether to draw the 50% / 85% threshold ticks.
  final bool warnBand;

  /// Bar height, px.
  final double height;

  @override
  Widget build(BuildContext context) {
    // Only the fill width is clamped (inside the painter); the ratio value is
    // allowed past 1 so the caller can render the true over-limit percentage.
    final safeRatio = ratio.isFinite && ratio > 0 ? ratio : 0.0;
    final clampedPeak = peak?.clamp(0.0, 1.0).toDouble();
    return SizedBox(
      height: height,
      child: CustomPaint(
        painter: VfdBarPainter(
          ratio: safeRatio,
          peak: clampedPeak,
          tint: tint,
          warnBand: warnBand,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}
