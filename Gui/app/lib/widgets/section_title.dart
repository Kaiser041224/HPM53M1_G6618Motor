import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Section heading matching the `.gui-section-title` motif.
///
/// All-caps wide-tracked accent title, optionally followed by a dim English
/// caption and a trailing widget, underlined by the "一亮两暗" hairline stack
/// (design-system §3.2).
class SectionTitle extends StatelessWidget {
  const SectionTitle({
    super.key,
    required this.title,
    this.en,
    this.trailing,
  });

  /// Primary (usually Chinese) title.
  final String title;

  /// Optional English caption, rendered uppercase in the label tier.
  final String? en;

  /// Optional trailing content, right-aligned.
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Container(
      padding: const EdgeInsets.only(bottom: 8),
      decoration: BoxDecoration(
        border: Border(
          bottom: BorderSide(color: palette.lineHi, width: palette.lineWidth),
        ),
        boxShadow: <BoxShadow>[
          BoxShadow(color: palette.lineDim, offset: const Offset(0, 2)),
          BoxShadow(color: palette.lineDim, offset: const Offset(0, 7)),
        ],
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.baseline,
        textBaseline: TextBaseline.alphabetic,
        children: <Widget>[
          Flexible(
            child: Text(
              title,
              style: scale.section,
              overflow: TextOverflow.ellipsis,
            ),
          ),
          if (en != null) ...[
            const SizedBox(width: 8),
            Text(en!.toUpperCase(), style: scale.label),
          ],
          const Spacer(),
          ?trailing,
        ],
      ),
    );
  }
}
