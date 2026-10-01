/// TNO 硅晶之梦输入框（v5 chrome）。
///
/// 外框、填充与无下划线由 [ThemeData.inputDecorationTheme] 统一供给（青
/// accent 1px、直角、bgRaise 填充）；本组件只补三件事：
/// - 聚焦时的青色弱辉光（`accent` 8px α.25，见设计系统「聚焦框亮青 + 辉光」）；
/// - 统一的输入文字（fgStrong，15px）；
/// - 控制器 / 键盘 / formatter / 图标等透传，避免每个调用点各自拼装
///   `InputDecoration` 而漂移出 Material 默认样式。
///
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
library;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../theme/gui_palette.dart';
import '../theme/gui_type.dart';

/// 聚焦辉光关键帧时长 [ms]。
const Duration kGuiTextFieldGlowMs = Duration(milliseconds: 120);

/// 聚焦辉光模糊半径 [logical px]。
const double kGuiTextFieldGlowBlur = 8;

/// 聚焦辉光不透明度（设计系统指定的弱辉光档）。
const double kGuiTextFieldGlowAlpha = 0.25;

/// 输入文字统一字号 [logical px]。
const double kGuiTextFieldFontSize = 15;

/// 深填充、青边、无下划线的输入框；聚焦时外圈亮青并附弱辉光。
class GuiTextField extends StatefulWidget {
  const GuiTextField({
    super.key,
    this.controller,
    this.enabled = true,
    this.autofocus = false,
    this.keyboardType,
    this.inputFormatters,
    this.hintText,
    this.labelText,
    this.prefixIcon,
    this.textAlign = TextAlign.start,
    this.style,
    this.onChanged,
    this.onSubmitted,
  });

  /// 文本控制器（可为空，内部自建）。
  final TextEditingController? controller;

  /// 是否可用；false 时禁用边框降为 lineDim，且不发光。
  final bool enabled;

  /// 是否自动聚焦（进入时即点亮辉光）。
  final bool autofocus;

  /// 键盘类型。
  final TextInputType? keyboardType;

  /// 输入过滤。
  final List<TextInputFormatter>? inputFormatters;

  /// 占位提示（fgDim）。
  final String? hintText;

  /// 浮动标签（fgDim → 聚焦 accent）。
  final String? labelText;

  /// 前置图标（18px，fgDim）。
  final IconData? prefixIcon;

  /// 文本对齐。
  final TextAlign textAlign;

  /// 输入文字样式覆盖；为 null 时用 fgStrong 15px。
  final TextStyle? style;

  /// 变更回调。
  final ValueChanged<String>? onChanged;

  /// 提交回调。
  final ValueChanged<String>? onSubmitted;

  @override
  State<GuiTextField> createState() => _GuiTextFieldState();
}

class _GuiTextFieldState extends State<GuiTextField> {
  final FocusNode _focusNode = FocusNode();
  bool _focused = false;

  @override
  void initState() {
    super.initState();
    _focusNode.addListener(_onFocusChanged);
  }

  void _onFocusChanged() {
    if (_focused != _focusNode.hasFocus) {
      setState(() => _focused = _focusNode.hasFocus);
    }
  }

  @override
  void dispose() {
    _focusNode.removeListener(_onFocusChanged);
    _focusNode.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final palette = GuiPalette.of(context);
    final scale = GuiTypeScale.of(context);
    final glow = _focused && widget.enabled;
    return AnimatedContainer(
      key: const Key('gui_text_field_glow'),
      duration: kGuiTextFieldGlowMs,
      decoration: BoxDecoration(
        boxShadow: glow
            ? <BoxShadow>[
                BoxShadow(
                  color: palette.accent.withValues(alpha: kGuiTextFieldGlowAlpha),
                  blurRadius: kGuiTextFieldGlowBlur,
                ),
              ]
            : const <BoxShadow>[],
      ),
      child: TextField(
        controller: widget.controller,
        focusNode: _focusNode,
        enabled: widget.enabled,
        autofocus: widget.autofocus,
        keyboardType: widget.keyboardType,
        inputFormatters: widget.inputFormatters,
        textAlign: widget.textAlign,
        onChanged: widget.onChanged,
        onSubmitted: widget.onSubmitted,
        style:
            widget.style ??
            scale.data.copyWith(
              fontSize: kGuiTextFieldFontSize,
              color: palette.fgStrong,
            ),
        decoration: InputDecoration(
          hintText: widget.hintText,
          labelText: widget.labelText,
          prefixIcon: widget.prefixIcon == null
              ? null
              : Icon(widget.prefixIcon, size: 18),
        ),
      ),
    );
  }
}
