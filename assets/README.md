# assets

`pbc_front.png` — the PicoBoy Color Plus from the front, rendered with Blender (Cycles) from
the board's 3D model (`~/Documents/pbcplus_de/pbcp.step`) by the pbcplus.de tooling
(`~/Documents/pbcplus_de/werkzeuge/render/render.py`, motif `vorne-buch`, file
`~/.cache/pbcplus-render/fertig/vorne-buch.png`), cropped to the board (offset 158, 221 from the
1116 x 1800 render). The build embeds it (`cmake/EmbedBinary.cmake`); `frontend/skin.c` draws the
LCD into it.

Geometry in this image's pixels (21.33 px per mm, an almost orthographic camera):

| | x | y | from |
|---|---|---|---|
| LCD active area (240 x 280) | 101.7 | 158.3 | 596.6 x 696.0 px = 27.97 x 32.63 mm (`SCHIRM` in render.py) |
| joystick knob | 197.5 | 1174.2 | `vorne-buch.json` "mitte" |
| button A | 482.1 | 1174.2 | "a" |
| button B | 638.7 | 1174.2 | "b" |
| LED red / yellow / green | 45.1 | 284.4 / 508.0 / 732.3 | "led_rot" ... |

A new render: run the motif again (`blender -b -P render.py -- vorne-buch out.png 1800`), crop
the same way and check the numbers with an overlay before replacing the file.
