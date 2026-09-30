import 'package:flutter/material.dart';

import '../theme/gui_glow.dart';
import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Status semantics, one color per state (design-system §6).
enum ItemState {
  /// Healthy / link ready.
  normal,

  /// Approaching a threshold.
  warn,

  /// Tripped.
  fault,

  /// Latched, awaiting an explicit clear.
  latched,

  /// Underlying value is stale.
  stale,

  /// Pending persistence.
  pending,
}

/// Status badge: color **and** icon **and** text, always all three.
///
/// Color must never carry the meaning alone (design-system §6, rule 1) — a
/// badge always renders a state glyph beside its label so it survives color
/// blindness, grayscale screenshots and projectors. The glow keeps the badge's
/// own hue and never cross-colorizes (design-system §2.4).
class StatusBadge extends StatelessWidget {
  const StatusBadge({super.key, required this.state, required this.label});

  /// Semantic state.
  final ItemState state;

  /// Human-readable label, rendered uppercase next to the glyph
  /// (design-system §5: all-caps; CJK is unaffected).
  final String label;

  /// The single glyph that encodes [state].
  static String iconFor(ItemState state) => switch (state) {
    ItemState.normal => '✓',
    ItemState.warn => '!',
    ItemState.fault => '✖',
    ItemState.latched => '⚠',
    ItemState.stale => '∅',
    ItemState.pending => '…',
  };

  /// The palette color for [state].
  static Color colorFor(GuiPalette palette, ItemState state) => switch (state) {
    ItemState.normal => palette.stNormal,
    ItemState.warn => palette.stWarn,
    ItemState.fault => palette.stFault,
    ItemState.latched => palette.stLatched,
    ItemState.stale => palette.stStale,
    ItemState.pending => palette.stPending,
  };

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final style = GuiGlowStyles.status(
      colorFor(palette, state),
      GuiTypeScale.of(context).label,
    );
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: <Widget>[
        Text(iconFor(state), style: style),
        const SizedBox(width: 4),
        Text(label.toUpperCase(), style: style),
      ],
    );
  }
}
