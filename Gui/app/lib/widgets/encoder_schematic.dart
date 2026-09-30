import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

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

/// Fraction of a measured angle delta that feeds the display pointer.
///
/// The dials are a *perceptual* readout — turn direction and a sense of speed,
/// not the literal crank angle — so a full measured revolution only nudges the
/// display pointer a twentieth of the way around.
const double kPointerSpeedScale = 0.05;

/// Ceiling on how fast a display pointer may advance [°/s].
const double kPointerMaxDps = 180;

/// Wraps a raw angle difference onto the shortest arc, in `(-180, 180]`.
double shortestArcDeg(double delta) {
  var wrapped = delta % 360;
  if (wrapped > 180) wrapped -= 360;
  return wrapped;
}

/// Tooth counts of the meshing gear pair for a reduction ratio [ratioLabel].
///
/// The input wheel is sized so the pair approximates the labelled ratio with
/// every count clamped to a legible band — `z1` inside `9..18`, `z2` inside
/// `12..28` — then the output count follows `z1 × ratio`. A larger ratio grows
/// the output wheel (up to the cap) and shrinks the input. An unloaded (`—`) or
/// unparseable label falls back to a `12 / 18` pair rather than a fabricated
/// 1:1 mesh.
(int, int) gearTeethPair(String ratioLabel) {
  final double? ratio = double.tryParse(ratioLabel);
  if (ratio == null || !ratio.isFinite || ratio <= 0) {
    return (12, 18);
  }
  final int input = (18 / ratio).round().clamp(9, 18);
  final int output = (input * ratio).round().clamp(12, 28);
  return (input, output);
}

/// One full input-wheel revolution — a constant `360° / 12 s == 30°/s`.
const Duration _inputWheelTurnPeriod = Duration(seconds: 12);

/// Output-wheel rotation [°] meshing-coupled to an input rotation.
///
/// External meshing reverses the sense and scales by the tooth ratio:
/// `θ_out = −θ_in · zInput / zOutput`. The pitch-line speeds therefore match,
/// so the input tooth stays seated in the output gap at every phase.
double coupledOutputGearAngleDeg(
  double inputAngleDeg,
  int zInput,
  int zOutput,
) => -inputAngleDeg * zInput / zOutput;

/// Line-drawn encoder schematic (v6, live meshing pair + screenshot colouring).
///
/// Reads left to right: a 360° dial for the rotor (`R · 转子`), a meshing gear
/// pair for the reduction, then a 360° dial for the output shaft (`O · 出轴`).
/// Each dial has a graduated ring (minor every 15°, major every 90° with a
/// numeral) and a pointer that rotates live with its angle — `0°` up, clockwise
/// positive. A null angle parks its pointer at 12 o'clock and pins a `—` badge
/// on the face.
///
/// The gear pair is drawn as two real involute-style wheels: addendum (tip),
/// dedendum (root) and dashed pitch circles, trapezoidal teeth (narrow tip, wide
/// root) meshing with a half-pitch phase offset, plus hubs with a bore, keyway
/// and four lightening holes. The input wheel is soft green, the output soft
/// violet; each body is low-alpha filled and rim-stroked. While the rotor
/// reading is live the pair turns as one rigid mesh — input at a constant visual
/// speed, output coupled in reverse by the tooth ratio — and honest-stops at its
/// frozen phase when the rotor goes stale. The pair is annotated
/// `i = {ratio} : 1` underneath (prefix `i =` fgDim, value amber; `i = —` when no
/// ratio is loaded). Every line and label colour comes from [GuiPalette].
class EncoderSchematic extends StatefulWidget {
  const EncoderSchematic({
    super.key,
    required this.rotorDeg,
    required this.outputDeg,
    required this.ratioLabel,
  });

  /// Rotor angle [°]; null when the reading is stale / never read.
  ///
  /// Also the gear-pair "live" predicate: a `null` rotor honest-stops the
  /// rotation in place, a non-null rotor runs it continuously.
  final double? rotorDeg;

  /// Output-shaft angle [°]; null when the reading is stale / never read.
  final double? outputDeg;

  /// Reduction-ratio label, e.g. `3.50`, or `—` when no ratio is loaded.
  final String ratioLabel;

  /// Drawing height; the width follows the parent.
  static const double height = 220;

  /// Key on the ratio annotation rich text (prefix + value spans).
  static const Key ratioLabelKey = Key('encoder_ratio_label');

  @override
  State<EncoderSchematic> createState() => EncoderSchematicState();
}

/// One dial pointer's perceptual integrator: the accumulated target angle the
/// display is converging on, the currently rendered angle, and the last raw
/// reading used to derive a delta.
class _PointerState {
  double? target;
  double? display;
  double? last;
}

/// Drives the meshing gear pair's rotation, integrates the perceptual dial
/// pointers and hosts the painting.
///
/// The input wheel turns at a constant visual speed (one revolution per
/// [_inputWheelTurnPeriod]); the output wheel follows the meshing-coupled
/// reversal. Independently, each dial pointer accumulates
/// `Δ × [kPointerSpeedScale]` of the measured angle change per frame, clamped
/// to [kPointerMaxDps] and lerped 25% toward that target — a *direction and
/// trend* cue, not a literal crank readout. A `null` reading freezes the
/// pointer honestly; the data columns elsewhere stay exact.
class EncoderSchematicState extends State<EncoderSchematic>
    with TickerProviderStateMixin {
  late final AnimationController _rotation;
  late final Ticker _pointerTicker;

  final _PointerState _rotorPointer = _PointerState();
  final _PointerState _outputPointer = _PointerState();
  Duration _lastTick = Duration.zero;

  /// Lerp fraction applied to each pointer per animation frame.
  @visibleForTesting
  static const double pointerLerp = 0.25;

  /// Current rendered rotor pointer angle [°]; null until the first reading.
  @visibleForTesting
  double? get displayRotorDeg => _rotorPointer.display;

  /// Current rendered output pointer angle [°]; null until the first reading.
  @visibleForTesting
  double? get displayOutputDeg => _outputPointer.display;

  @override
  void initState() {
    super.initState();
    _rotation = AnimationController(
      vsync: this,
      duration: _inputWheelTurnPeriod,
    );
    _pointerTicker = createTicker(_onPointerTick);
    _seedPointers();
    _syncRotation();
    _syncTicker();
  }

  @override
  void didUpdateWidget(covariant EncoderSchematic oldWidget) {
    super.didUpdateWidget(oldWidget);
    _seedPointers();
    _syncRotation();
    _syncTicker();
  }

  /// Snaps a never-rendered pointer directly to the current reading, so the
  /// first frame paints at the measured angle instead of 0°.
  void _seedPointers() {
    _seedPointer(_rotorPointer, widget.rotorDeg);
    _seedPointer(_outputPointer, widget.outputDeg);
  }

  void _seedPointer(_PointerState pointer, double? reading) {
    if (pointer.display == null && reading != null) {
      pointer.display = reading;
      pointer.target = reading;
      pointer.last = reading;
    }
  }

  /// Runs the pair while the rotor is live; honest-stops it on `null`.
  ///
  /// [AnimationController.repeat] resumes from the current value, so a
  /// stale → live transition picks the gears back up at the frozen phase.
  void _syncRotation() {
    if (widget.rotorDeg == null) {
      _rotation.stop();
    } else if (!_rotation.isAnimating) {
      _rotation.repeat();
    }
  }

  /// Runs the pointer integrator while either dial is live.
  void _syncTicker() {
    final live = widget.rotorDeg != null || widget.outputDeg != null;
    if (live && !_pointerTicker.isActive) {
      _lastTick = Duration.zero;
      _pointerTicker.start();
    } else if (!live && _pointerTicker.isActive) {
      _pointerTicker.stop();
    }
  }

  /// Advances both dial pointers for one animation frame.
  ///
  /// [elapsed] is measured from the ticker start; the per-frame `dt` is clamped
  /// so a scheduler hiccup cannot inject an unbounded jump.
  void _onPointerTick(Duration elapsed) {
    final dt =
        ((elapsed - _lastTick).inMicroseconds / Duration.microsecondsPerSecond)
            .clamp(0.0, 0.1);
    _lastTick = elapsed;
    _advancePointer(_rotorPointer, widget.rotorDeg, dt);
    _advancePointer(_outputPointer, widget.outputDeg, dt);
    if (mounted) setState(() {});
  }

  /// Integrates one pointer from its measured [reading] over [dt] seconds.
  ///
  /// A `null` reading freezes the display in place (and forgets the last
  /// reading so a stale → live transition re-snaps rather than lurching). The
  /// first-ever reading snaps the display to the measured angle.
  void _advancePointer(_PointerState pointer, double? reading, double dt) {
    if (reading == null) {
      pointer.last = null;
      return;
    }
    final last = pointer.last;
    if (pointer.display == null || last == null) {
      pointer.last = reading;
      pointer.display = reading;
      pointer.target = reading;
      return;
    }
    final delta = shortestArcDeg(reading - last);
    pointer.last = reading;
    final maxStep = kPointerMaxDps * dt;
    final advance = (delta * kPointerSpeedScale).clamp(-maxStep, maxStep);
    pointer.target = pointer.target! + advance;
    pointer.display = pointer.display! + (pointer.target! - pointer.display!) * pointerLerp;
  }

  @override
  void dispose() {
    _pointerTicker.dispose();
    _rotation.dispose();
    super.dispose();
  }

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
    final ratioPrefixStyle = scale.label.copyWith(
      color: palette.fgDim,
      fontSize: 10,
    );
    final ratioValueStyle = scale.label.copyWith(
      color: widget.ratioLabel == '—' ? palette.fgMute : palette.valueAmber,
      fontSize: 10,
    );
    final ratioValue = widget.ratioLabel == '—'
        ? '—'
        : '${widget.ratioLabel} : 1';

    return SizedBox(
      height: EncoderSchematic.height,
      child: LayoutBuilder(
        builder: (context, constraints) {
          final size = Size(constraints.maxWidth, constraints.maxHeight);
          final radius = EncoderSchematicPainter.dialRadius(size);
          final rotorCenter = EncoderSchematicPainter.rotorCenter(size);
          final outputCenter = EncoderSchematicPainter.outputCenter(size);
          final gearCenter = EncoderSchematicPainter.gearCenter(size);
          final gearTip = EncoderSchematicPainter.gearTipRadiusFor(
            size,
            widget.ratioLabel,
          );

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
                child: AnimatedBuilder(
                  animation: _rotation,
                  builder: (context, _) {
                    final double inputAngleDeg = _rotation.value * 360;
                    final (int zInput, int zOutput) = gearTeethPair(
                      widget.ratioLabel,
                    );
                    return CustomPaint(
                      painter: EncoderSchematicPainter(
                        rotorDeg: _rotorPointer.display,
                        outputDeg: _outputPointer.display,
                        ratioLabel: widget.ratioLabel,
                        inputAngleDeg: inputAngleDeg,
                        outputGearAngleDeg: coupledOutputGearAngleDeg(
                          inputAngleDeg,
                          zInput,
                          zOutput,
                        ),
                        accent: palette.accent,
                        line: palette.line,
                        fgDim: palette.fgDim,
                        valueAmber: palette.valueAmber,
                        gearGreen: palette.gearGreen,
                        gearGreenRim: palette.gearGreenRim,
                        gearViolet: palette.gearViolet,
                        gearVioletRim: palette.gearVioletRim,
                        bgBase: palette.bgBase,
                        glowSoft: palette.glowSoft,
                      ),
                    );
                  },
                ),
              ),
              caption('R · 转子', const Key('encoder_dial_rotor'), rotorCenter),
              caption('O · 出轴', const Key('encoder_dial_output'), outputCenter),
              if (widget.rotorDeg == null)
                nodata(rotorCenter, const Key('encoder_dial_rotor_nodata')),
              if (widget.outputDeg == null)
                nodata(outputCenter, const Key('encoder_dial_output_nodata')),
              Positioned(
                left: gearCenter.dx - 70,
                top: gearCenter.dy + gearTip + 18,
                width: 140,
                child: Text.rich(
                  TextSpan(
                    children: <TextSpan>[
                      TextSpan(text: 'i = ', style: ratioPrefixStyle),
                      TextSpan(text: ratioValue, style: ratioValueStyle),
                    ],
                  ),
                  key: EncoderSchematic.ratioLabelKey,
                  textAlign: TextAlign.center,
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
/// Dials stay flat 1.2px wireframes (cyan for `R`, amber for `O`); the gear pair
/// is a soft-filled, rim-stroked realistic mesh. The only glow is a small
/// [glowSoft] dot at each pointer tip.
class EncoderSchematicPainter extends CustomPainter {
  const EncoderSchematicPainter({
    required this.rotorDeg,
    required this.outputDeg,
    required this.ratioLabel,
    this.inputAngleDeg = 0,
    this.outputGearAngleDeg = 0,
    required this.accent,
    required this.line,
    required this.fgDim,
    required this.valueAmber,
    required this.gearGreen,
    required this.gearGreenRim,
    required this.gearViolet,
    required this.gearVioletRim,
    required this.bgBase,
    required this.glowSoft,
  });

  /// Rotor *display* pointer angle [°] (scaled/lerped for perception); null
  /// parks the pointer at 12 o'clock.
  final double? rotorDeg;

  /// Output-shaft *display* pointer angle [°]; null parks the pointer at
  /// 12 o'clock.
  final double? outputDeg;

  /// Reduction-ratio label, mirrored into the gear tooth counts.
  final String ratioLabel;

  /// Live rotation of the input (green) wheel [°], driven by the widget tick.
  ///
  /// Read-only hook for the animation; it also sets the painted phase of the
  /// wheel's teeth, hub and lightening holes.
  final double inputAngleDeg;

  /// Meshing-coupled rotation of the output (violet) wheel [°].
  ///
  /// Held at `−inputAngleDeg · z1 / z2` so the teeth never leave the mesh.
  final double outputGearAngleDeg;

  /// Rotor (`R`) dial ring + pointer colour, soft cyan.
  final Color accent;

  /// Thin dark seam / dashed pitch-circle colour.
  final Color line;

  /// Graduation / caption / prefix colour.
  final Color fgDim;

  /// Output (`O`) dial ring + pointer colour, olive amber; also the ratio value.
  final Color valueAmber;

  /// Input gear soft fill.
  final Color gearGreen;

  /// Input gear deep rim.
  final Color gearGreenRim;

  /// Output gear soft fill.
  final Color gearViolet;

  /// Output gear deep rim.
  final Color gearVioletRim;

  /// Page base colour, used to punch out bores, keyways and lightening holes.
  final Color bgBase;

  /// Single soft glow colour, used only at the pointer tips.
  final Color glowSoft;

  /// Thin wire weight (design-system: 1.2px, never scaled for emphasis).
  static const double strokeWidth = 1.2;

  /// Pointer length as a fraction of the dial radius.
  static const double pointerRadiusFactor = 0.78;

  /// Minor graduation step [°].
  static const double minorTickStepDeg = 15;

  /// Major graduation step [°] (carries a numeral).
  static const double majorTickStepDeg = 90;

  /// Gear tip (addendum) radius as a multiple of the pitch radius.
  static const double addendumFactor = 1.16;

  /// Gear root (dedendum) radius as a multiple of the pitch radius.
  static const double dedendumFactor = 0.84;

  /// Hub outer radius as a multiple of the pitch radius.
  static const double hubFactor = 0.30;

  /// Centre bore radius as a multiple of the pitch radius.
  static const double hubBoreFactor = 0.13;

  /// Lightening-hole radius as a multiple of the pitch radius.
  static const double lighteningHoleFactor = 0.15;

  /// Lightening-hole orbit radius as a multiple of the pitch radius.
  static const double lighteningHoleOrbitFactor = 0.55;

  /// Number of circular lightening holes drilled in each wheel.
  static const int lighteningHoleCount = 4;

  /// Dashed pitch-circle stroke width.
  static const double pitchCircleWidth = 0.6;

  /// Body-fill alpha for a gear (soft, translucent).
  static const double gearFillAlpha = 0.22;

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

  /// Half-width budget of the whole meshing pair within [size].
  static double gearRadius(Size size) =>
      math.min(size.width * 0.13, size.height * 0.20);

  /// Pitch radii `(input, output)` of the pair for [size] / [ratioLabel].
  ///
  /// Both share one module so `rInput : rOutput == z1 : z2`; the centre distance
  /// is chosen so the addendum circles fill (but never exceed) [gearRadius].
  static (double, double) pitchRadiiFor(Size size, String ratioLabel) {
    final (int zInput, int zOutput) = gearTeethPair(ratioLabel);
    final double half = gearRadius(size);
    final double distance = half * 2 / (addendumFactor + 1);
    final int total = zInput + zOutput;
    return (distance * zInput / total, distance * zOutput / total);
  }

  /// X offsets `(input, output)` of the two wheel centres within [size].
  static (double, double) gearCentersXFor(Size size, String ratioLabel) {
    final (double rInput, double rOutput) = pitchRadiiFor(size, ratioLabel);
    final double half = gearRadius(size);
    final double cx = gearCenter(size).dx;
    return (
      cx - half + rInput * addendumFactor,
      cx + half - rOutput * addendumFactor,
    );
  }

  /// Tip (addendum) radius of the pair's largest wheel for [size].
  static double gearTipRadiusFor(Size size, String ratioLabel) {
    final (double rInput, double rOutput) = pitchRadiiFor(size, ratioLabel);
    return addendumFactor * math.max(rInput, rOutput);
  }

  /// Mutable helper: a stroke [Paint] of [color] / [width].
  Paint _stroke(Color color, double width) => Paint()
    ..style = PaintingStyle.stroke
    ..strokeWidth = width
    ..strokeCap = StrokeCap.round
    ..isAntiAlias = true
    ..color = color;

  /// Mutable helper: a solid fill [Paint] of [color].
  Paint _fill(Color color) => Paint()
    ..style = PaintingStyle.fill
    ..isAntiAlias = true
    ..color = color;

  @override
  void paint(Canvas canvas, Size size) {
    final radius = dialRadius(size);
    final rotorCenter = EncoderSchematicPainter.rotorCenter(size);
    final outputCenter = EncoderSchematicPainter.outputCenter(size);
    final gearCenter = EncoderSchematicPainter.gearCenter(size);

    _drawDial(canvas, rotorCenter, radius, rotorDeg, accent, fgDim, line);
    _drawDial(canvas, outputCenter, radius, outputDeg, valueAmber, fgDim, line);

    _drawGearPair(canvas, size, gearCenter);

    final y = rotorCenter.dy;
    final half = gearRadius(size);
    _drawConnector(
      canvas,
      Offset(rotorCenter.dx + radius + 8, y),
      Offset(gearCenter.dx - half - 8, y),
      _stroke(fgDim, 1),
    );
    _drawConnector(
      canvas,
      Offset(gearCenter.dx + half + 8, y),
      Offset(outputCenter.dx - radius - 8, y),
      _stroke(fgDim, 1),
    );
  }

  /// Draws one graduated dial and its pointer in [channel] colour.
  ///
  /// Passing [channel] separately lets the rotor dial read cyan while the
  /// output dial reads amber.
  void _drawDial(
    Canvas canvas,
    Offset center,
    double radius,
    double? deg,
    Color channel,
    Color faintColor,
    Color seamColor,
  ) {
    final faint = _stroke(faintColor, 1);
    canvas.drawCircle(center, radius, _stroke(channel, strokeWidth));

    for (double tick = 0; tick < 360; tick += minorTickStepDeg) {
      final bool major = tick % majorTickStepDeg == 0;
      final inner = radius - (major ? 9 : 4);
      canvas.drawLine(
        pointerTip(center, inner, tick),
        pointerTip(center, radius, tick),
        faint,
      );
      if (major) {
        _drawTickNumeral(canvas, center, radius - 17, tick, faintColor);
      }
    }

    final angle = deg ?? 0.0;
    final tip = pointerTip(center, radius * pointerRadiusFactor, angle);
    canvas.drawCircle(tip, radius * 0.06, _fill(glowSoft));
    final pointer = _stroke(channel, strokeWidth);
    canvas.drawLine(center, tip, pointer);
    final head = radius * 0.16;
    canvas.drawLine(tip, pointerTip(tip, head, angle + 152), pointer);
    canvas.drawLine(tip, pointerTip(tip, head, angle - 152), pointer);
    canvas.drawCircle(center, radius * 0.07, _stroke(seamColor, 1));
  }

  /// Draws the `0 / 90 / 180 / 270` numeral at dial angle [deg].
  void _drawTickNumeral(
    Canvas canvas,
    Offset center,
    double radius,
    double deg,
    Color color,
  ) {
    final painter = TextPainter(
      text: TextSpan(
        text: deg.toInt().toString(),
        style: TextStyle(color: color, fontSize: 7),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    final at = pointerTip(center, radius, deg);
    painter.paint(canvas, at - Offset(painter.width / 2, painter.height / 2));
  }

  /// Draws the meshing gear pair, centres on the shaft line.
  void _drawGearPair(Canvas canvas, Size size, Offset gearCenter) {
    final (int zInput, int zOutput) = gearTeethPair(ratioLabel);
    final (double rInput, double rOutput) = pitchRadiiFor(size, ratioLabel);
    final (double xInput, double xOutput) = gearCentersXFor(size, ratioLabel);
    final double y = gearCenter.dy;
    final double inputRotation = inputAngleDeg * math.pi / 180;
    final double outputRotation = outputGearAngleDeg * math.pi / 180;

    // Input wheel: a tooth centred on the pitch point (angle 0), turned by the
    // live input phase.
    _drawGear(
      canvas,
      Offset(xInput, y),
      rInput,
      zInput,
      0,
      inputRotation,
      gearGreen,
      gearGreenRim,
    );
    // Output wheel: a gap centred on the pitch point (angle π), i.e. phase
    // shifted by half a tooth pitch so the two teeth interlock, then turned by
    // the meshing-coupled output phase.
    _drawGear(
      canvas,
      Offset(xOutput, y),
      rOutput,
      zOutput,
      math.pi - math.pi / zOutput,
      outputRotation,
      gearViolet,
      gearVioletRim,
    );
  }

  /// Draws one real gear wheel: toothed body, hubs, bore, keyway, holes.
  ///
  /// [rotation] turns the whole wheel — teeth, lightening holes and keyway —
  /// as one rigid body about its centre.
  void _drawGear(
    Canvas canvas,
    Offset center,
    double pitchRadius,
    int teeth,
    double phase,
    double rotation,
    Color fillColor,
    Color rimColor,
  ) {
    canvas.save();
    canvas.translate(center.dx, center.dy);
    canvas.rotate(rotation);

    final outline = _gearOutline(Offset.zero, pitchRadius, teeth, phase);

    // Soft body fill, then the deep rim that carries the silhouette.
    canvas.drawPath(outline, _fill(fillColor.withValues(alpha: gearFillAlpha)));
    final rim = _stroke(rimColor, strokeWidth);
    canvas.drawPath(outline, rim);

    // Four circular lightening holes punched through the web.
    final holeRadius = pitchRadius * lighteningHoleFactor;
    final orbit = pitchRadius * lighteningHoleOrbitFactor;
    final holeOutline = _stroke(rimColor, 1);
    for (var i = 0; i < lighteningHoleCount; i++) {
      final angle = math.pi / 4 + i * math.pi / 2;
      final hole = Offset(math.cos(angle), math.sin(angle)) * orbit;
      canvas.drawCircle(hole, holeRadius, _fill(bgBase));
      canvas.drawCircle(hole, holeRadius, holeOutline);
    }

    // Hub outer circle.
    final hubRadius = pitchRadius * hubFactor;
    canvas.drawCircle(Offset.zero, hubRadius, _stroke(rimColor, 1));

    // Centre bore + keyway, punched to the page base and rimmed in the wheel
    // colour.
    final boreRadius = pitchRadius * hubBoreFactor;
    final keywayOutline = _stroke(rimColor, 1);
    canvas.drawCircle(Offset.zero, boreRadius, _fill(bgBase));
    canvas.drawCircle(Offset.zero, boreRadius, keywayOutline);
    final keyway = Rect.fromLTRB(
      -boreRadius * 0.35,
      -hubRadius,
      boreRadius * 0.35,
      -boreRadius * 0.4,
    );
    canvas.drawRect(keyway, _fill(bgBase));
    canvas.drawRect(keyway, keywayOutline);

    // Dashed pitch circle.
    _drawDashedCircle(
      canvas,
      Offset.zero,
      pitchRadius,
      _stroke(line, pitchCircleWidth),
    );

    canvas.restore();
  }

  /// Builds the closed, filled outline of a gear with [teeth] trapezoidal teeth.
  ///
  /// Tip land is narrower than the root (an involute approximation) and the root
  /// gaps are sampled on the dedendum circle for a rounded look.
  Path _gearOutline(
    Offset center,
    double pitchRadius,
    int teeth,
    double phase,
  ) {
    final tipRadius = pitchRadius * addendumFactor;
    final rootRadius = pitchRadius * dedendumFactor;
    final pitch = 2 * math.pi / teeth;
    final halfTooth = math.pi / (2 * teeth);
    final tipHalf = halfTooth * 0.55;
    final rootHalf = halfTooth * 1.45;

    final path = Path();
    for (var i = 0; i < teeth; i++) {
      final theta = phase + i * pitch;
      final rootLeft = _polar(center, rootRadius, theta - rootHalf);
      if (i == 0) {
        path.moveTo(rootLeft.dx, rootLeft.dy);
      } else {
        path.lineTo(rootLeft.dx, rootLeft.dy);
      }
      final tipLeft = _polar(center, tipRadius, theta - tipHalf);
      final tipRight = _polar(center, tipRadius, theta + tipHalf);
      final rootRight = _polar(center, rootRadius, theta + rootHalf);
      path.lineTo(tipLeft.dx, tipLeft.dy);
      path.lineTo(tipRight.dx, tipRight.dy);
      path.lineTo(rootRight.dx, rootRight.dy);

      // Sample the root gap across to the next tooth.
      final nextRootLeft = theta + pitch - rootHalf;
      const int samples = 3;
      for (var s = 1; s <= samples; s++) {
        final angle =
            theta + rootHalf + (nextRootLeft - theta - rootHalf) * s / samples;
        final point = _polar(center, rootRadius, angle);
        path.lineTo(point.dx, point.dy);
      }
    }
    path.close();
    return path;
  }

  /// Point at [radius] and [angle] around [center].
  Offset _polar(Offset center, double radius, double angle) =>
      center + Offset(math.cos(angle), math.sin(angle)) * radius;

  /// Draws a dashed circle of [radius] around [center] with [paint].
  void _drawDashedCircle(
    Canvas canvas,
    Offset center,
    double radius,
    Paint paint,
  ) {
    const int dashes = 40;
    final step = 2 * math.pi / dashes;
    final rect = Rect.fromCircle(center: center, radius: radius);
    for (var i = 0; i < dashes; i++) {
      canvas.drawArc(rect, i * step, step * 0.55, false, paint);
    }
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
      canvas.drawLine(from + direction * start, from + direction * end, paint);
    }
  }

  @override
  bool shouldRepaint(covariant EncoderSchematicPainter oldDelegate) =>
      oldDelegate.rotorDeg != rotorDeg ||
      oldDelegate.outputDeg != outputDeg ||
      oldDelegate.ratioLabel != ratioLabel ||
      oldDelegate.inputAngleDeg != inputAngleDeg ||
      oldDelegate.outputGearAngleDeg != outputGearAngleDeg ||
      oldDelegate.accent != accent ||
      oldDelegate.line != line ||
      oldDelegate.fgDim != fgDim ||
      oldDelegate.valueAmber != valueAmber ||
      oldDelegate.gearGreen != gearGreen ||
      oldDelegate.gearGreenRim != gearGreenRim ||
      oldDelegate.gearViolet != gearViolet ||
      oldDelegate.gearVioletRim != gearVioletRim ||
      oldDelegate.bgBase != bgBase ||
      oldDelegate.glowSoft != glowSoft;
}
