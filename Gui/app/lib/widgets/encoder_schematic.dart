import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Maps a dial angle [deg] to canvas radians.
///
/// The dial convention is human: `0°` is straight up (12 o'clock) and positive
/// angles run clockwise, matching how an encoder reading reads on a face.
double dialRadians(double deg) => (deg - 90) * math.pi / 180;

/// The tip of a pointer of [radius] around [center] at dial angle [deg].
Offset pointerTip(Offset center, double radius, double deg) {
  final radians = dialRadians(deg);
  return center + Offset(math.cos(radians), math.sin(radians)) * radius;
}

/// Tooth counts of the meshing gear pair for a reduction ratio [ratioLabel].
///
/// The pair approximates the labelled ratio (a larger output ratio gets a
/// larger output gear) with every count clamped to `8..20` so the sketch stays
/// legible; an unloaded (`—`) or unparseable label falls back to a 1:1 mesh.
(int, int) gearTeethPair(String ratioLabel) {
  const int base = 14;
  final double? ratio = double.tryParse(ratioLabel);
  if (ratio == null || !ratio.isFinite || ratio <= 0) {
    return (base, base);
  }
  final int output = (base * ratio).round().clamp(8, 20);
  return (base, output);
}

/// Line-drawn encoder schematic (v4, three-part thin wireframe).
///
/// Reads left to right: a 360° dial for the rotor (`R · 转子`), a meshing gear
/// pair for the reduction, then a 360° dial for the output shaft (`O · 出轴`).
/// Each dial has a graduated ring (minor every 15°, major every 90° with a
/// numeral) and a pointer that rotates live with its angle — `0°` up, clockwise
/// positive. A null angle parks its pointer at 12 o'clock and pins a `—` badge
/// on the face. The gear pair is annotated `i = {ratio} : 1` underneath
/// (`i = —` when no ratio is loaded). Every line and label colour comes from
/// [GuiPalette]; strokes stay 1.2px flat, with no glow.
class EncoderSchematic extends StatelessWidget {
  const EncoderSchematic({
    super.key,
    required this.rotorDeg,
    required this.outputDeg,
    required this.ratioLabel,
  });

  /// Rotor angle [°]; null when the reading is stale / never read.
  final double? rotorDeg;

  /// Output-shaft angle [°]; null when the reading is stale / never read.
  final double? outputDeg;

  /// Reduction-ratio label, e.g. `3.50`, or `—` when no ratio is loaded.
  final String ratioLabel;

  /// Drawing height; the width follows the parent.
  static const double height = 220;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final captionStyle = scale.label.copyWith(
      color: palette.fgDim,
      fontSize: 9,
    );
    final nodataStyle = scale.data.copyWith(
      color: palette.fgMute,
      fontSize: 16,
      shadows: const <Shadow>[],
    );
    final ratioStyle = scale.label.copyWith(
      color: ratioLabel == '—' ? palette.fgMute : palette.valueAmber,
      fontSize: 10,
    );
    final ratioText = ratioLabel == '—' ? 'i = —' : 'i = $ratioLabel : 1';

    return SizedBox(
      height: height,
      child: LayoutBuilder(
        builder: (context, constraints) {
          final size = Size(constraints.maxWidth, constraints.maxHeight);
          final radius = EncoderSchematicPainter.dialRadius(size);
          final rotorCenter = EncoderSchematicPainter.rotorCenter(size);
          final outputCenter = EncoderSchematicPainter.outputCenter(size);
          final gearCenter = EncoderSchematicPainter.gearCenter(size);
          final gearRadius = EncoderSchematicPainter.gearRadius(size);

          Widget caption(String text, Key key, Offset center) => Positioned(
            left: center.dx - 60,
            top: center.dy + radius + 8,
            width: 120,
            child: Text(
              text,
              key: key,
              textAlign: TextAlign.center,
              style: captionStyle,
            ),
          );

          Widget nodata(Offset center, Key key) => Positioned(
            left: center.dx - 16,
            top: center.dy - 12,
            width: 32,
            child: Text(
              '—',
              key: key,
              textAlign: TextAlign.center,
              style: nodataStyle,
            ),
          );

          return Stack(
            clipBehavior: Clip.none,
            children: <Widget>[
              Positioned.fill(
                child: CustomPaint(
                  painter: EncoderSchematicPainter(
                    rotorDeg: rotorDeg,
                    outputDeg: outputDeg,
                    ratioLabel: ratioLabel,
                    accent: palette.accent,
                    line: palette.line,
                    fgDim: palette.fgDim,
                    valueAmber: palette.valueAmber,
                  ),
                ),
              ),
              caption(
                'R · 转子',
                const Key('encoder_dial_rotor'),
                rotorCenter,
              ),
              caption(
                'O · 出轴',
                const Key('encoder_dial_output'),
                outputCenter,
              ),
              if (rotorDeg == null)
                nodata(rotorCenter, const Key('encoder_dial_rotor_nodata')),
              if (outputDeg == null)
                nodata(outputCenter, const Key('encoder_dial_output_nodata')),
              Positioned(
                left: gearCenter.dx - 70,
                top: gearCenter.dy + gearRadius + 20,
                width: 140,
                child: Text(
                  ratioText,
                  textAlign: TextAlign.center,
                  style: ratioStyle,
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

/// Paints the three-part schematic: rotor dial, meshing gear pair, output dial.
///
/// Flat 1.2px strokes only — the ring, graduations and gears are a technical
/// wireframe, never a filled or glowing shape.
class EncoderSchematicPainter extends CustomPainter {
  const EncoderSchematicPainter({
    required this.rotorDeg,
    required this.outputDeg,
    required this.ratioLabel,
    required this.accent,
    required this.line,
    required this.fgDim,
    required this.valueAmber,
  });

  /// Rotor angle [°]; null parks the pointer at 12 o'clock.
  final double? rotorDeg;

  /// Output-shaft angle [°]; null parks the pointer at 12 o'clock.
  final double? outputDeg;

  /// Reduction-ratio label, mirrored into the gear tooth counts.
  final String ratioLabel;

  /// Primary wire colour (rotor dial ring, pointers, gears).
  final Color accent;

  /// Secondary seam colour (gear hubs).
  final Color line;

  /// Graduation / caption colour.
  final Color fgDim;

  /// Output pointer colour.
  final Color valueAmber;

  /// Thin wire weight (design-system: 1.2px, never scaled for emphasis).
  static const double strokeWidth = 1.2;

  /// Pointer length as a fraction of the dial radius.
  static const double pointerRadiusFactor = 0.78;

  /// Minor graduation step [°].
  static const double minorTickStepDeg = 15;

  /// Major graduation step [°] (carries a numeral).
  static const double majorTickStepDeg = 90;

  /// Centre of the rotor dial within [size].
  static Offset rotorCenter(Size size) =>
      Offset(size.width * 0.16, size.height * 0.42);

  /// Centre of the output dial within [size].
  static Offset outputCenter(Size size) =>
      Offset(size.width * 0.84, size.height * 0.42);

  /// Radius of either dial within [size].
  static double dialRadius(Size size) =>
      math.min(size.width * 0.13, size.height * 0.30);

  /// Centre of the gear pair within [size].
  static Offset gearCenter(Size size) =>
      Offset(size.width * 0.5, size.height * 0.40);

  /// Radius of each gear within [size].
  static double gearRadius(Size size) =>
      math.min(size.width * 0.055, size.height * 0.13);

  @override
  void paint(Canvas canvas, Size size) {
    final wire = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = strokeWidth
      ..strokeCap = StrokeCap.round
      ..isAntiAlias = true
      ..color = accent;
    final faint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1
      ..strokeCap = StrokeCap.round
      ..isAntiAlias = true
      ..color = fgDim;
    final seam = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1
      ..strokeCap = StrokeCap.round
      ..isAntiAlias = true
      ..color = line;
    final amberWire = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = strokeWidth
      ..strokeCap = StrokeCap.round
      ..isAntiAlias = true
      ..color = valueAmber;

    final radius = dialRadius(size);
    final rotorCenter = EncoderSchematicPainter.rotorCenter(size);
    final outputCenter = EncoderSchematicPainter.outputCenter(size);
    final gearCenter = EncoderSchematicPainter.gearCenter(size);
    final gearR = EncoderSchematicPainter.gearRadius(size);

    _drawDial(canvas, rotorCenter, radius, rotorDeg, wire, amberWire, faint, seam);
    _drawDial(
      canvas,
      outputCenter,
      radius,
      outputDeg,
      wire,
      amberWire,
      faint,
      seam,
    );

    _drawGearPair(canvas, gearCenter, gearR, wire, seam);

    final y = rotorCenter.dy;
    _drawConnector(
      canvas,
      Offset(rotorCenter.dx + radius + 8, y),
      Offset(gearCenter.dx - gearR * 2 - 8, y),
      faint,
    );
    _drawConnector(
      canvas,
      Offset(gearCenter.dx + gearR * 2 + 8, y),
      Offset(outputCenter.dx - radius - 8, y),
      faint,
    );
  }

  /// Draws one graduated dial and its pointer.
  ///
  /// [pointerColor] is passed separately so the output dial can read amber
  /// while the rotor dial stays accent-cyan.
  void _drawDial(
    Canvas canvas,
    Offset center,
    double radius,
    double? deg,
    Paint ring,
    Paint pointerColor,
    Paint faint,
    Paint hub,
  ) {
    canvas.drawCircle(center, radius, ring);

    for (
      double tick = 0;
      tick < 360;
      tick += minorTickStepDeg
    ) {
      final bool major = tick % majorTickStepDeg == 0;
      final inner = radius - (major ? 9 : 4);
      canvas.drawLine(
        pointerTip(center, inner, tick),
        pointerTip(center, radius, tick),
        faint,
      );
      if (major) {
        _drawTickNumeral(canvas, center, radius - 17, tick);
      }
    }

    final angle = deg ?? 0.0;
    final tip = pointerTip(center, radius * pointerRadiusFactor, angle);
    canvas.drawLine(center, tip, pointerColor);
    final head = radius * 0.16;
    canvas.drawLine(tip, pointerTip(tip, head, angle + 152), pointerColor);
    canvas.drawLine(tip, pointerTip(tip, head, angle - 152), pointerColor);
    canvas.drawCircle(center, radius * 0.07, hub);
  }

  /// Draws the `0 / 90 / 180 / 270` numeral at dial angle [deg].
  void _drawTickNumeral(Canvas canvas, Offset center, double radius, double deg) {
    final painter = TextPainter(
      text: TextSpan(
        text: deg.toInt().toString(),
        style: TextStyle(color: fgDim, fontSize: 7),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    final at = pointerTip(center, radius, deg);
    painter.paint(canvas, at - Offset(painter.width / 2, painter.height / 2));
  }

  /// Draws the meshing gear pair: two toothed wheels offset half a tooth pitch.
  void _drawGearPair(
    Canvas canvas,
    Offset center,
    double radius,
    Paint wire,
    Paint hub,
  ) {
    final (int leftTeeth, int rightTeeth) = gearTeethPair(ratioLabel);
    final gap = radius;
    _drawGear(canvas, center - Offset(gap, 0), radius, leftTeeth, 0, wire, hub);
    _drawGear(
      canvas,
      center + Offset(gap, 0),
      radius,
      rightTeeth,
      math.pi / rightTeeth,
      wire,
      hub,
    );
  }

  /// Draws one gear wheel with [teeth] radial teeth at [phase] radians.
  void _drawGear(
    Canvas canvas,
    Offset center,
    double radius,
    int teeth,
    double phase,
    Paint wire,
    Paint hub,
  ) {
    canvas.drawCircle(center, radius, wire);
    final pitch = 2 * math.pi / teeth;
    final toothLength = radius * 0.22;
    for (var i = 0; i < teeth; i++) {
      final a = phase + i * pitch;
      final outward = Offset(math.cos(a), math.sin(a));
      canvas.drawLine(
        center + outward * radius,
        center + outward * (radius + toothLength),
        wire,
      );
    }
    canvas.drawCircle(center, radius * 0.16, hub);
  }

  /// Draws a short dashed connector between two joints on the shaft line.
  void _drawConnector(Canvas canvas, Offset from, Offset to, Paint paint) {
    const double dash = 7;
    const double gap = 5;
    final total = (to - from).distance;
    if (total <= 0) return;
    final direction = (to - from) / total;
    for (double start = 0; start < total; start += dash + gap) {
      final end = math.min(start + dash, total);
      canvas.drawLine(
        from + direction * start,
        from + direction * end,
        paint,
      );
    }
  }

  @override
  bool shouldRepaint(covariant EncoderSchematicPainter oldDelegate) =>
      oldDelegate.rotorDeg != rotorDeg ||
      oldDelegate.outputDeg != outputDeg ||
      oldDelegate.ratioLabel != ratioLabel ||
      oldDelegate.accent != accent ||
      oldDelegate.line != line ||
      oldDelegate.fgDim != fgDim ||
      oldDelegate.valueAmber != valueAmber;
}
