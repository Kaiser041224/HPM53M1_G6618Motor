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
/// - connected: the 38x5 block glyphs from [artLines], one stop color per row
///   ([artColorStops]), matching the firmware terminal exactly (§4.4).
class DeviceAscii extends StatelessWidget {
  const DeviceAscii({super.key, required this.connected, this.modelName});

  /// Whether a device identity has been read.
  final bool connected;

  /// Model string from the `PROTO_INFO` handshake, shown when available.
  final String? modelName;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    if (!connected) {
      return DashedFrame(
        color: palette.lineDim,
        child: SizedBox(
          width: 190,
          height: 62,
          child: Center(
            child: Text(
              'DEVICE ID PENDING',
              style: scale.label.copyWith(color: palette.fgMute),
            ),
          ),
        ),
      );
    }
    final glyph = TextStyle(
      fontFamily: GuiTypeScale.monoFamily,
      fontFamilyFallback: GuiTypeScale.monoFallback,
      fontSize: 15,
      height: 1.08,
      letterSpacing: 0,
      color: palette.fgStrong,
    );
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        for (var i = 0; i < artLines.length; i++)
          Text(
            artLines[i],
            style: GuiGlowStyles.ascii(artColorStops[i], glyph),
          ),
        if (modelName != null) ...<Widget>[
          const SizedBox(height: 6),
          Text(
            modelName!,
            style: scale.label.copyWith(color: palette.fgDim),
          ),
        ],
      ],
    );
  }
}
