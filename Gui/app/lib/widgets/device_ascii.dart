import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import 'device_art_data.dart';
import 'stale_gate.dart';

/// Device identity block art, or a pending placeholder while disconnected.
///
/// The model is read from the device, never assumed (design-system §4.3):
///
/// - disconnected: a dashed box reading `DEVICE ID PENDING`;
/// - connected: the 38x5 block glyphs from [artLines], painted as a fixed cell
///   grid ([DeviceArtPainter]) so the shape is independent of the font that
///   carries U+2588. One stop color per row ([artColorStops]), matching the
///   firmware terminal exactly (§4.4).
class DeviceAscii extends StatelessWidget {
  const DeviceAscii({super.key, required this.connected});

  /// Whether a device identity has been read.
  final bool connected;

  /// Logical cell width [px] — ~8 at 1x. The whole grid scales by changing
  /// these two constants.
  static const double cellWidth = 7;

  /// Logical cell height [px] — ~12 at 1x.
  static const double cellHeight = 11;

  /// Reserved padding around the grid so the row glow can bleed without
  /// clipping or overlapping neighbours.
  static const double glowPadding = 10;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    if (!connected) {
      return DashedFrame(
        color: palette.lineDim,
        child: SizedBox(
          width: 190,
          height: 62,
          child: Center(
            child: Text(
              'DEVICE ID PENDING',
              style: GuiTypeScale.of(
                context,
              ).label.copyWith(color: palette.fgMute),
            ),
          ),
        ),
      );
    }
    final columns = artRows.isEmpty ? 0 : artRows.first.length;
    return SizedBox(
      width: columns * cellWidth + glowPadding * 2,
      height: artRows.length * cellHeight + glowPadding * 2,
      child: CustomPaint(
        painter: DeviceArtPainter(
          rows: artRows,
          stops: artColorStops,
          cellWidth: cellWidth,
          cellHeight: cellHeight,
          padding: glowPadding,
        ),
      ),
    );
  }
}

/// Paints the device art as a cell grid: one solid rect per `█`, a top/bottom
/// half rect for `▀` / `▄`, and nothing for a space.
///
/// Each row is drawn as a single [Path] with the legacy three-layer ASCII glow
/// ([GuiGlowStyles.ascii]) sourced from its own stop color, then the solid fill.
/// Painting geometry instead of text keeps the glyph geometry and line widths
/// exact regardless of the active font.
class DeviceArtPainter extends CustomPainter {
  const DeviceArtPainter({
    required this.rows,
    required this.stops,
    required this.cellWidth,
    required this.cellHeight,
    this.padding = 0,
  });

  /// Row-major cell grid (5 rows x 38 columns for the G6618 banner).
  final List<List<ArtCell>> rows;

  /// Per-row stop colors; row `i` uses `stops[i]`.
  final List<Color> stops;

  /// Cell width [px].
  final double cellWidth;

  /// Cell height [px].
  final double cellHeight;

  /// Padding [px] reserved on every side for the glow.
  final double padding;

  /// The same-hue glow tiers used by the ASCII device art (three layers).
  static List<Shadow> glowFor(Color stop) =>
      GuiGlowStyles.ascii(stop, const TextStyle()).shadows!;

  @override
  void paint(Canvas canvas, Size size) {
    for (var r = 0; r < rows.length; r++) {
      if (r >= stops.length) break;
      final stop = stops[r];
      final path = Path();
      final row = rows[r];
      final top = padding + r * cellHeight;
      for (var c = 0; c < row.length; c++) {
        final cell = row[c];
        if (cell == ArtCell.empty) continue;
        final left = padding + c * cellWidth;
        switch (cell) {
          case ArtCell.full:
            path.addRect(Rect.fromLTWH(left, top, cellWidth, cellHeight));
          case ArtCell.upperHalf:
            path.addRect(
              Rect.fromLTWH(left, top, cellWidth, cellHeight / 2),
            );
          case ArtCell.lowerHalf:
            path.addRect(
              Rect.fromLTWH(left, top + cellHeight / 2, cellWidth, cellHeight / 2),
            );
          case ArtCell.empty:
            break;
        }
      }
      for (final shadow in glowFor(stop)) {
        canvas.drawPath(
          path,
          Paint()
            ..color = shadow.color
            ..maskFilter = ui.MaskFilter.blur(
              ui.BlurStyle.normal,
              shadow.blurRadius,
            ),
        );
      }
      canvas.drawPath(path, Paint()..color = stop);
    }
  }

  @override
  bool shouldRepaint(covariant DeviceArtPainter oldDelegate) =>
      oldDelegate.rows != rows ||
      oldDelegate.stops != stops ||
      oldDelegate.cellWidth != cellWidth ||
      oldDelegate.cellHeight != cellHeight ||
      oldDelegate.padding != padding;
}
