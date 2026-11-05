# Open Pad Glyphs

Controller button glyphs at the Switch's private-use button characters (U+E000-E153: A / B / X / Y, L / R / ZL / ZR /
SL / SR, sticks, d-pad, + / -, HOME, Capture, arrows), drawn from scratch for the NintendoExt shared font when the
console's own isn't imported (runtime/hle/pl.c).

- `OpenPadGlyphs.ttf`: what the app loads. The Switch's letters on the console's codepoints, plus, for each controller
  family (Nintendo, Xbox, PlayStation) and controller button, the glyph of every character that names one Switch input
  (at U+F0000 + family << 14 + button << 9 + (character - U+E000)). The app rewrites the font's cmap at start so those
  characters show the button bound to that input, in the connected controller's labels. By default (by position):
  east = A = Xbox B = circle, south = B = Xbox A = cross, north = X = Y = triangle, west = Y = X = square,
  L / R = LB / RB = L1 / R1, ZL / ZR = LT / RT = L2 / R2, + / - = Menu / View = Start / Select.
- `OpenPadGlyphs-SwitchLabels.ttf`, `-XboxLabels.ttf`, `-PSLabels.ttf`: the same glyphs as fixed sets at the default
  binding, for a runtime that loads a font as it is.

The font is monochrome (the game draws it in the text colour): no Xbox / PlayStation button colours.

Source: `build_padglyphs.py` (fontTools + skia-pathops) draws every glyph; the button letters are Baloo 2 Bold outlines
(`fetch_sources.sh` downloads the pinned Baloo 2 from google/fonts). No console font is used. To rebuild:

    python3 -m venv venv && venv/bin/pip install fonttools skia-pathops
    ./fetch_sources.sh && venv/bin/python build_padglyphs.py

License: SIL Open Font License 1.1 (`OFL.txt`).
