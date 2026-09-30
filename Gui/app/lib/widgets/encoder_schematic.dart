import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Line-drawn dual-encoder schematic (v4, thin cyan wireframe).
///
/// A FACOM-style thin-line technical drawing: a horizontal motor cylinder
/// (`G66-18`) pierced by the through shaft, with one small encoder box at each
/// end — `R` on the rotor side, `O` on the output side — each tied to a small
/// label by a leader line. The centre annotates the gear ratio (`×0.980`, or
/// `×—` when the parameter is unloaded). Lines are 1.2px, flat, no glow; every
/// colour comes from [GuiPalette] (`accent` / `line` / `fgDim`) so nothing is
/// hard-coded.
class EncoderSchematic extends StatelessWidget {
  const EncoderSchematic({super.key, required this.ratio});

  /// Output-shaft gear ratio, or null when the parameter is unloaded (`×—`).
  final double? ratio;

  /// Drawing height; the width follows the parent.
  static const double height = 220;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final labelStyle = scale.label.copyWith(color: palette.fgDim);
    final ratioStyle = scale.body.copyWith(fontSize: 11, color: palette.accent);

    return SizedBox(
      height: height,
      child: Stack(
        children: <Widget>[
          Positioned.fill(
            child: CustomPaint(
              painter: EncoderSchematicPainter(
                accent: palette.accent,
                line: palette.line,
                fgDim: palette.fgDim,
              ),
            ),
          ),
          _at(Alignment(0, 0.04), Text('G66-18', style: labelStyle)),
          _at(
            Alignment(0, -0.66),
            Text(
              ratio == null ? '×—' : '×${ratio!.toStringAsFixed(3)}',
              style: ratioStyle,
            ),
          ),
          _at(
            Alignment(-0.66, 0.80),
            Text('R · 转子 KTH7823', style: labelStyle),
          ),
          _at(
            Alignment(0.66, 0.80),
            Text('O · 出轴 KTH7823', style: labelStyle),
          ),
        ],
      ),
    );
  }

  Widget _at(Alignment alignment, Widget child) =>
      Align(alignment: alignment, child: child);
}

/// Paints the dual-encoder line drawing: through shaft, motor cylinder body,
/// two encoder boxes and their leader lines. Flat 1.2px strokes, no glow.
class EncoderSchematicPainter extends CustomPainter {
  const EncoderSchematicPainter({
    required this.accent,
    required this.line,
    required this.fgDim,
  });

  /// Primary wire colour (bright-cyan hairline token).
  final Color accent;

  /// Secondary seam colour (regular border token).
  final Color line;

  /// Leader-line / faint detail colour.
  final Color fgDim;

  /// Thin wire weight (design-system: 1.2px, never scaled for emphasis).
  static const double strokeWidth = 1.2;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width;
    final h = size.height;
    final yAxis = h * 0.52;

    final body = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = strokeWidth
      ..strokeCap = StrokeCap.round
      ..isAntiAlias = true
      ..color = accent;
    final seam = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1
      ..strokeCap = StrokeCap.round
      ..isAntiAlias = true
      ..color = line;
    final leader = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1
      ..isAntiAlias = true
      ..color = fgDim;

    // Through shaft.
    canvas.drawLine(Offset(w * 0.10, yAxis), Offset(w * 0.90, yAxis), body);

    // Motor cylinder body.
    final left = w * 0.32;
    final right = w * 0.68;
    final top = h * 0.30;
    final bottom = h * 0.74;
    final faceHeight = bottom - top;
    canvas.drawLine(Offset(left, top), Offset(right, top), body);
    canvas.drawLine(Offset(left, bottom), Offset(right, bottom), body);
    final faceWidth = w * 0.055;
    final centerY = (top + bottom) / 2;
    canvas.drawOval(
      Rect.fromCenter(
        center: Offset(left, centerY),
        width: faceWidth,
        height: faceHeight,
      ),
      body,
    );
    canvas.drawOval(
      Rect.fromCenter(
        center: Offset(right, centerY),
        width: faceWidth,
        height: faceHeight,
      ),
      body,
    );
    for (var i = 1; i <= 3; i++) {
      final x = left + (right - left) * i / 4;
      canvas.drawLine(
        Offset(x, top + faceHeight * 0.14),
        Offset(x, bottom - faceHeight * 0.14),
        seam,
      );
    }

    // Encoder boxes at each shaft end.
    final leftBox = Rect.fromCenter(
      center: Offset(w * 0.13, yAxis),
      width: w * 0.09,
      height: h * 0.22,
    );
    final rightBox = Rect.fromCenter(
      center: Offset(w * 0.87, yAxis),
      width: w * 0.09,
      height: h * 0.22,
    );
    canvas.drawRect(leftBox, body);
    canvas.drawRect(rightBox, body);

    // Leader lines from each encoder box down to its label.
    canvas.drawLine(
      Offset(leftBox.center.dx, leftBox.bottom),
      Offset(w * 0.20, h * 0.84),
      leader,
    );
    canvas.drawLine(
      Offset(rightBox.center.dx, rightBox.bottom),
      Offset(w * 0.80, h * 0.84),
      leader,
    );

    // Leader from the ratio annotation down to the shaft centre.
    canvas.drawLine(
      Offset(w * 0.5, h * 0.20),
      Offset(w * 0.5, yAxis - 6),
      leader,
    );
  }

  @override
  bool shouldRepaint(covariant EncoderSchematicPainter oldDelegate) =>
      oldDelegate.accent != accent ||
      oldDelegate.line != line ||
      oldDelegate.fgDim != fgDim;
}
