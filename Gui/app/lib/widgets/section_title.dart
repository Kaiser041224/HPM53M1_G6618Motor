import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import 'hairline_frame.dart';

/// v5 unified title-bar gradient: violet → magenta → transparent, left to
/// right. The exact stop sequence is taken from the approved sample
/// `Gui/ref/design-system/previews/gui-style-sample.html` (`.panel-title`,
/// `.mini-title`): `rgba(138,77,255,.38)` → `rgba(224,64,200,.12)` →
/// `transparent`.
const LinearGradient kTitleGradient = LinearGradient(
  begin: Alignment.centerLeft,
  end: Alignment.centerRight,
  colors: <Color>[
    Color(0x618A4DFF),
    Color(0x1FE040C8),
    Color(0x008A4DFF),
  ],
  stops: <double>[0.0, 0.5, 1.0],
);

/// v5 title glow: one weak cyan halo (`.glow-cy`, blur 6). Titles keep
/// [GuiPalette.fgStrong] as their fill; the halo is the only light they carry.
const List<Shadow> kTitleGlow = <Shadow>[
  Shadow(color: Color(0x592ED8E8), blurRadius: 6),
];

/// Applies the shared v5 title glow to a base title style.
TextStyle guiTitleStyle(TextStyle base) => base.copyWith(shadows: kTitleGlow);

/// Shared title-bar chrome for [SectionTitle] and `PanelTitle` (v5).
///
/// Paints the unified violet→magenta→transparent gradient behind a vertically
/// padded [child] (6–8px) and closes it with the role-fixed low-bright violet
/// group line. Centralising the chrome here keeps every title on the page
/// identical — titles only differ in their type tier.
class GuiTitleFrame extends StatelessWidget {
  const GuiTitleFrame({super.key, required this.child});

  /// The title row (title text + optional English caption / trailing widget).
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Container(
          key: const Key('gui_title_bar'),
          decoration: BoxDecoration(
            gradient: kTitleGradient,
            // 与内容外框同色系的青色锐利边框（accent，与 HairlineFrame 同档 1.5px），
            // 包住整条渐变标题条 —— 全页标题风格统一。
            border: Border.all(
              color: palette.accent,
              width: HairlineFrame.brightWidth,
            ),
          ),
          // 文字距渐变底左右缘 ≥10px 且垂直居中：不顶格、不溢出背景（修正旧版顶格）。
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
          child: child,
        ),
        Container(
          key: const Key('gui_title_underline'),
          height: palette.lineWidth,
          color: palette.groupLine,
        ),
      ],
    );
  }
}

/// Section heading (v5).
///
/// A solid white-cyan Chinese title ([GuiTypeScale.sectionTitle]) with a weak
/// cyan glow, an optional small Latin caption, and a trailing widget, set on
/// the shared gradient [GuiTitleFrame] and closed by the low-brightness violet
/// group line ([GuiPalette.groupLine], §2.6.2).
class SectionTitle extends StatelessWidget {
  const SectionTitle({
    super.key,
    required this.title,
    this.en,
    this.trailing,
  });

  /// Primary (usually Chinese) title, rendered big and white-cyan.
  final String title;

  /// Optional English caption, rendered uppercase in the label tier.
  final String? en;

  /// Optional trailing content, right-aligned.
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return GuiTitleFrame(
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
              style: guiTitleStyle(scale.sectionTitle),
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
    );
  }
}
