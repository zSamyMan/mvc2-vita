# Notices

## No game data

This repository holds source code only. The game's program, graphics and sound come from the user's own copy,
read at start-up: the files of the Dreamcast disc.

## port/naomi/naomi_m1.c

The M1 cartridge's packed-file scheme is as worked out in MAME's `naomim1.cpp` (BSD-3-Clause,
copyright-holders: Olivier Galibert).

## Libraries

`port/ext/libchdr` is [libchdr](https://github.com/rtissera/libchdr) (BSD-3-Clause), for reading a disc image
in a CHD, with the decoders it carries: the LZMA SDK's (public domain), miniz (Unlicense), zstd's
(BSD-3-Clause, or GPLv2) and dr_flac (public domain, or MIT-0). Their licenses are in their files there.

## PS Vita build

The PS Vita build (`mvc2dc.vpk`) links these libraries statically, as VitaSDK's packages (`vdpm`) build them,
unmodified. Their license texts are in `licenses/`, and the VPK carries this file and those texts in its own
`licenses/` folder.

| Library | Version linked | License | Source |
|---|---|---|---|
| [vitaGL](https://github.com/Rinnegatamante/vitaGL), copyright Rinnegatamante | `2bdbe89` (vdpm `0.0.0.r1488.g2bdbe89-1`) | LGPL-3.0 (`licenses/LGPL-3.0.txt`, with `licenses/GPL-3.0.txt`) | https://github.com/Rinnegatamante/vitaGL/tree/2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8 |
| [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK), copyright Rinnegatamante | 1.7 | LGPL-3.0 | https://github.com/Rinnegatamante/vitaShaRK |
| [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) | 1.0.1 | LGPL-2.1 (`licenses/LGPL-2.1.txt`) | https://github.com/bythos14/SceShaccCgExt |
| [math-neon](https://github.com/Rinnegatamante/math-neon), copyright 2015 Lachlan Tychsen-Smith | `0faab81` | MIT (`licenses/math-neon-MIT.txt`) | https://github.com/Rinnegatamante/math-neon |
| [SDL2](https://libsdl.org) for the Vita | VitaSDK's | zlib | https://github.com/libsdl-org/SDL |

The port uses vitaGL, vitaShaRK and SceShaccCgExt only through their published interfaces; none of their code
is copied into this repository. As the GNU LGPL asks of a work that links a library statically, the
program's complete source is this repository (GNU AGPL-3.0), so anyone can rebuild it against a modified
vitaGL (or vitaShaRK, or SceShaccCgExt) and install the result. `README.md`'s PS Vita section shows how.

The shader compiler that vitaShaRK drives, Sony's `libshacccg.suprx`, is not included: it is loaded at run
time from the user's own `ur0:data/` (or `ur0:data/external/`), if it is there. The VPK's `shaders/pack.bin`
holds the port's own shaders (`port/render/rd_vgl.c`), compiled ahead of time.

Marvel vs. Capcom 2 and Capcom are trademarks of Capcom; Marvel characters are trademarks of Marvel. This
project is not affiliated with or endorsed by Capcom, Marvel or Sega.
