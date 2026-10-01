import 'package:flutter/material.dart';

import '../theme/gui_type.dart';
import '../widgets/hairline_frame.dart';
import '../widgets/section_title.dart';

// Re-export the title hue vocabulary so panel call sites need only one import.
export '../widgets/section_title.dart' show TitleHue, kDefaultTitleHue;

/// Light sub-panel heading (v5).
///
/// A smaller sibling of [SectionTitle]: a white-cyan semibold Chinese title
/// with the shared weak cyan glow, an optional small uppercase Latin caption
/// and an optional trailing widget, set on the same gradient [GuiTitleFrame]
/// and closed by the single low-brightness violet group line
/// ([GuiPalette.groupLine], §2.6.2). Sharing [GuiTitleFrame] keeps the whole
/// page's titles identical without duplicating the chrome.
class PanelTitle extends StatelessWidget {
  const PanelTitle({
    super.key,
    required this.title,
    this.en,
    this.trailing,
    this.hue = kDefaultTitleHue,
  });

  /// Primary (usually Chinese) title, rendered white-cyan and semibold.
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

/// 一体化面板：外层统一 [HairlineFrame]（青 1.5px）包裹「标题条 + 内容区」。
///
/// 标题条（[PanelTitle] 的渐变底 + 低亮紫下沿）**贴齐外框内缘**，与内容之间
/// 以标题条自身的下沿作为唯一分隔，形成无缝整体；内容区四周保留 [contentPadding]。
/// 复刻样张 `.mini-panel` / `.panel-title`（`gui-style-sample.html`）的负外边距效果，
/// 但用零内边距 + 内容侧内边距实现，避免负 margin 的布局副作用。
class PanelBox extends StatelessWidget {
  const PanelBox({
    super.key,
    this.frameKey,
    required this.title,
    this.en,
    this.trailing,
    this.hue = kDefaultTitleHue,
    required this.child,
    this.contentPadding = const EdgeInsets.all(10),
  });

  /// [HairlineFrame] 的 key（沿用旧面板 key，避免调用方 / 测试漂移）。
  final Key? frameKey;

  /// 面板标题（同 [PanelTitle]）。
  final String title;

  /// 可选英文副标。
  final String? en;

  /// 可选右侧内容。
  final Widget? trailing;

  /// 标题条渐变色相（v6）。
  final TitleHue hue;

  /// 标题条之下的内容区。
  final Widget child;

  /// 内容区内边距（默认 10）。
  final EdgeInsetsGeometry contentPadding;

  @override
  Widget build(BuildContext context) {
    return HairlineFrame(
      key: frameKey,
      padding: EdgeInsets.zero,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          PanelTitle(title: title, en: en, trailing: trailing, hue: hue),
          Padding(padding: contentPadding, child: child),
        ],
      ),
    );
  }
}
