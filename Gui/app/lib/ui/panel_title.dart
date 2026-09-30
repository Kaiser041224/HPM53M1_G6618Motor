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
        Flexible(
          child: Text(
            title,
            style: scale.sectionTitle.copyWith(fontSize: 13),
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
    );
  }
}
