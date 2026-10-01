import 'package:flutter/material.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';
import 'hairline_frame.dart';

/// Title-bar hue families (v6).
///
/// Every title keeps the same gradient *structure* — hue @38% → hue @12% →
/// transparent, left to right — but may pick a hue so adjacent headings read as
/// a sequence instead of one monochrome wall. The hue only reinforces the
/// title text, which is always present, so colour never carries meaning alone
/// (design-system §6 rule 1).
enum TitleHue {
  /// Violet — the approved sample family (default).
  violet,

  /// Magenta.
  magenta,

  /// Green.
  green,

  /// Amber.
  amber,

  /// Cyan.
  cyan,
}

/// Resolves the palette colour behind [hue].
Color titleHueColor(GuiPalette palette, TitleHue hue) => switch (hue) {
  TitleHue.violet => palette.valuePurple,
  TitleHue.magenta => palette.valueMagenta,
  TitleHue.green => palette.valueGreen,
  TitleHue.amber => palette.valueAmber,
  TitleHue.cyan => palette.accent,
};

/// Default title hue — violet, the family of the approved sample.
const TitleHue kDefaultTitleHue = TitleHue.violet;

/// Builds the v6 gradient for [hue] from [palette]: the hue at 38%, the hue at
/// 12%, then fully transparent — the same three-stop structure as the original
/// violet sample, only parameterised by hue.
LinearGradient titleGradientFor(GuiPalette palette, TitleHue hue) {
  final Color c = titleHueColor(palette, hue);
  return LinearGradient(
    begin: Alignment.centerLeft,
    end: Alignment.centerRight,
    colors: <Color>[
      c.withValues(alpha: 0.38),
      c.withValues(alpha: 0.12),
      c.withValues(alpha: 0.0),
    ],
    stops: const <double>[0.0, 0.5, 1.0],
  );
}

/// The default ([TitleHue.violet]) gradient as an ARGB constant, kept for
/// callers / tests that predate the hue parameter.
const LinearGradient kTitleGradient = LinearGradient(
  begin: Alignment.centerLeft,
  end: Alignment.centerRight,
  colors: <Color>[
    Color(0x618A4DFF),
    Color(0x1F8A4DFF),
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
  const GuiTitleFrame({
    super.key,
    required this.child,
    this.hue = kDefaultTitleHue,
  });

  /// The title row (title text + optional English caption / trailing widget).
  final Widget child;

  /// Gradient hue family (v6). Defaults to violet.
  final TitleHue hue;

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
            gradient: titleGradientFor(palette, hue),
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
    this.hue = kDefaultTitleHue,
  });

  /// Primary (usually Chinese) title, rendered big and white-cyan.
  final String title;

  /// Optional English caption, rendered uppercase in the label tier.
  final String? en;

  /// Optional trailing content, right-aligned.
  final Widget? trailing;

  /// Gradient hue family (v6).
  final TitleHue hue;

  @override
  Widget build(BuildContext context) {
    final scale = GuiTypeScale.of(context);
    return GuiTitleFrame(
      hue: hue,
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
