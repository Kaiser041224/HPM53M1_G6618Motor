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

/// One cell of the device art grid.
///
/// The art is rendered as a fixed cell grid rather than a text run, so it is
/// independent of whichever font happens to carry U+2588: a `█` becomes one
/// solid cell, `▀` / `▄` a half cell and a space stays transparent.
enum ArtCell {
  /// Transparent cell.
  empty,

  /// Full block `█` (U+2588).
  full,

  /// Upper half block `▀` (U+2580).
  upperHalf,

  /// Lower half block `▄` (U+2584).
  lowerHalf,
}

/// Maps one art character to its [ArtCell]. Anything that is not a block glyph
/// is transparent, so a trailing space and a missing cell render identically.
ArtCell artCellForChar(String ch) => switch (ch) {
  '█' => ArtCell.full,
  '▀' => ArtCell.upperHalf,
  '▄' => ArtCell.lowerHalf,
  _ => ArtCell.empty,
};

/// Parses [lines] into a row-major [ArtCell] grid. Every line keeps its own
/// length, so a short line shows as trailing transparent cells.
List<List<ArtCell>> parseDeviceArt(List<String> lines) => <List<ArtCell>>[
  for (final line in lines)
    <ArtCell>[
      for (final rune in line.runes)
        artCellForChar(String.fromCharCode(rune)),
    ],
];

/// The device art as a derived cell grid (5 rows x 38 columns), parsed from
/// [artLines] at load time. Never hand-edit: it stays in lockstep with the
/// firmware banner via [artLines].
final List<List<ArtCell>> artRows = parseDeviceArt(artLines);
