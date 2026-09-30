import 'package:flutter/material.dart';

/// 38-column x 5-row UTF-8 block glyphs for the device name.
///
/// Extracted verbatim from the firmware USB terminal banner
/// (`App/Comm/terminal/Src/app_terminal.c`, rows 619-623) via
/// `Gui/ref/tools/extract-device-art.ps1`. Do not hand-edit: the Gui identity
/// must stay byte-identical to the on-device banner (design-system 4.4).
///
/// @author  Kaiser
/// Copyright (c) 2026 Alliance HardwareGroup
/// SPDX-License-Identifier: BSD-3-Clause
const List<String> artLines = <String>[
  ' ██████   ██████   ██████   ██  █████ ',
  '██       ██       ██       ███ ██   ██',
  '██   ███ ███████  ███████   ██  █████ ',
  '██    ██ ██    ██ ██    ██  ██ ██   ██',
  ' ██████   ██████   ██████   ██  █████ ',
];

/// Per-row colors, terminal 256-color order 45 -> 51 -> 141 -> 207 -> 213.
const List<Color> artColorStops = <Color>[
  Color(0xFF00D7FF),
  Color(0xFF00FFFF),
  Color(0xFFAF87FF),
  Color(0xFFFF5FFF),
  Color(0xFFFF87FF),
];
