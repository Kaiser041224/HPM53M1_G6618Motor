import 'package:flutter/material.dart';

import '../theme/gui_type.dart';

/// Light sub-panel heading (v3).
///
/// A smaller sibling of `SectionTitle`: a white semibold Chinese title, an
/// optional small uppercase Latin caption and an optional trailing widget —
/// but no bright underline, so the scarce divider motif stays reserved for the
/// three main zones (design-system §3.3).
class PanelTitle extends StatelessWidget {
  const PanelTitle({super.key, required this.title, this.en, this.trailing});

  /// Primary (usually Chinese) title, rendered white and semibold.
  final String title;

  /// Optional English caption, rendered uppercase in the label tier.
  final String? en;

  /// Optional trailing content, right-aligned.
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return Row(
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: <Widget>[
        // **flex 必须远大于 Spacer**。
        //
        // 两者若都是 flex:1，会平分剩余空间，标题只拿到一半 ——
        // 窄栏（如合并页右栏的控制面板）下中文标题因此被省略号截掉。
        // 给标题 99 对 1 的权重，既保住「标题尽量展开」，
        // 又让 trailing 仍被推到最右。
        Flexible(
          flex: 99,
          child: Text(
            title,
            // 中文必须落在像素网格上（Fusion Pixel 只在原生尺寸锐利），
            // 故不再压到 13，直接用刻度里的 12。
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
    );
  }
}
