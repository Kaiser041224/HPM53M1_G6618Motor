import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// Section heading (v3).
///
/// A solid white Chinese title ([GuiTypeScale.sectionTitle]), an optional
/// small Latin caption, and a trailing widget, underlined by a single bright
/// cyan 1px line.
class SectionTitle extends StatelessWidget {
  const SectionTitle({
    super.key,
    required this.title,
    this.en,
    this.trailing,
  });

  /// Primary (usually Chinese) title, rendered big and white.
  final String title;

  /// Optional English caption, rendered uppercase in the label tier.
  final String? en;

  /// Optional trailing content, right-aligned.
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Padding(
          padding: const EdgeInsets.only(bottom: 8),
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: <Widget>[
              // flex 99:1 —— 与 PanelTitle 同理：Flexible 与 Spacer 等权时
              // 会平分剩余空间，标题只拿到一半而被截断。
              Flexible(
                flex: 99,
                child: Text(
                  title,
                  style: scale.sectionTitle,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
              ),
              if (en != null) ...[
                const SizedBox(width: 8),
                Text(en!.toUpperCase(), style: scale.label),
              ],
              if (trailing != null) ...[
                const Spacer(),
                trailing!,
              ],
            ],
          ),
        ),
        Container(height: palette.lineWidth, color: palette.lineHi),
      ],
    );
  }
}
