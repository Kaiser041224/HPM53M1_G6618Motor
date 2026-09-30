import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Button emphasis tiers (v3).
enum GuiButtonVariant {
  /// Default action: neutral hairline.
  normal,

  /// Main action: cyan border / text.
  primary,

  /// Destructive action: red border / text.
  danger,
}

/// The resolved fill / border / text / glow for a button state.
@immutable
class GuiButtonStyle {
  const GuiButtonStyle({
    required this.fill,
    required this.border,
    required this.text,
    required this.glow,
  });

  /// Interior fill.
  final Color fill;

  /// Border color.
  final Color border;

  /// Label / icon color.
  final Color text;

  /// Hover glow layers (empty when idle or disabled).
  final List<BoxShadow> glow;
}

/// Square, hairline button in three emphasis tiers (v3).
///
/// Height is 30, corners are square, and the fill is the shared
/// [GuiPalette.btnFill]. Hover brightens the border to [GuiPalette.lineHi]
/// (or [GuiPalette.stFault] for [GuiButtonVariant.danger]) and adds one very
/// faint same-hue glow. Disabled buttons dim to [GuiPalette.fgMute].
class GuiButton extends StatefulWidget {
  const GuiButton({
    super.key,
    required this.label,
    this.variant = GuiButtonVariant.normal,
    this.icon,
    this.onPressed,
  });

  /// Button label.
  final String label;

  /// Emphasis tier.
  final GuiButtonVariant variant;

  /// Optional leading icon.
  final IconData? icon;

  /// Tap callback; null renders the disabled state.
  final VoidCallback? onPressed;

  /// Fixed button height.
  static const double height = 30;

  /// Resolves the styling for [variant] and [hovered] / [enabled].
  static GuiButtonStyle styleFor(
    GuiPalette palette,
    GuiButtonVariant variant, {
    bool hovered = false,
    bool enabled = true,
  }) {
    final (Color border, Color text) = switch (variant) {
      GuiButtonVariant.normal => (palette.btnBorder, palette.btnText),
      GuiButtonVariant.primary => (
        palette.btnPrimaryBorder,
        palette.btnPrimaryText,
      ),
      GuiButtonVariant.danger => (palette.btnDangerBorder, palette.btnDangerText),
    };
    if (!enabled) {
      return GuiButtonStyle(
        fill: palette.btnFill,
        border: border,
        text: palette.fgMute,
        glow: const <BoxShadow>[],
      );
    }
    final hoverBorder = variant == GuiButtonVariant.danger
        ? palette.stFault
        : palette.lineHi;
    return GuiButtonStyle(
      fill: palette.btnFill,
      border: hovered ? hoverBorder : border,
      text: text,
      glow: hovered
          ? <BoxShadow>[
              BoxShadow(
                color: hoverBorder.withValues(alpha: 0.22),
                blurRadius: 8,
              ),
            ]
          : const <BoxShadow>[],
    );
  }

  @override
  State<GuiButton> createState() => _GuiButtonState();
}

class _GuiButtonState extends State<GuiButton> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final enabled = widget.onPressed != null;
    final style = GuiButton.styleFor(
      palette,
      widget.variant,
      hovered: _hovered,
      enabled: enabled,
    );
    final textStyle = GuiTypeScale.of(
      context,
    ).body.copyWith(color: style.text, fontSize: 12);
    return MouseRegion(
      cursor: enabled ? SystemMouseCursors.click : SystemMouseCursors.basic,
      onEnter: (_) => setState(() => _hovered = true),
      onExit: (_) => setState(() => _hovered = false),
      child: GestureDetector(
        onTap: widget.onPressed,
        child: Container(
          constraints: const BoxConstraints.tightFor(height: GuiButton.height),
          padding: const EdgeInsets.symmetric(horizontal: 12),
          decoration: BoxDecoration(
            color: style.fill,
            border: Border.all(color: style.border, width: 1),
            boxShadow: style.glow,
          ),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              if (widget.icon != null) ...[
                Icon(widget.icon, size: 14, color: style.text),
                const SizedBox(width: 6),
              ],
              Text(widget.label, style: textStyle),
            ],
          ),
        ),
      ),
    );
  }
}
