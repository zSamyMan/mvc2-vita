# Marvel vs. Capcom 2 for the PS Vita

**AI Disclosure**:
Yes, this used Claude.

A native PS Vita port of *Marvel vs. Capcom 2: New Age of Heroes* (the Dreamcast version), at 60 fps. It is not
an emulator: the game's decompiled C is compiled for the Vita's ARM CPU, and the port supplies what the
Dreamcast and Sega's libraries did, with vitaGL drawing on the Vita's GPU.

**No game data is included.** The game's program, graphics and sound come from your own copy of the USA
Dreamcast disc, read at start-up.

## Where it comes from

The game code under `src/` and `include/` is the decompilation of the NAOMI arcade and Dreamcast releases by
Giblet (`mvc2-naomi-dc`), which rebuilds both programs byte for byte. This
repository keeps only what the Vita port needs from it, and does not aim to contribute back to it: the
decompilation's own toolchain, tests and documentation live in that project.

## Playing

1. A PS Vita or PS TV with HENkaku/Enso, taiHEN, [kubridge](https://github.com/bythos14/kubridge) and the
   shader compiler (`libshacccg.suprx` in `ur0:data/`).
2. Install `mvc2dc.vpk` with VitaShell.
3. Put the USA disc's files (`1ST_READ.BIN` and the rest), or an image of the disc (`.chd`, `.gdi`, `.cue`,
   or an `.iso` of track 3), in `ux0:data/mvc_2/disc/`. An image is unpacked into that folder at the first start.

## Building

See [ports/README.md](ports/README.md): VitaSDK with vitaGL, then CMake and Ninja.

## Layout

    ports/         the Vita port: its CMake build, the host (board, disc, sound, renderer) and its notices
    src/           the decompiled game code, a file for each of Capcom's (src/dc: the Dreamcast's own)
    include/       the decompilation's headers
    mvc2.sub       the programs' link order and section addresses (mvc2dc.sub for the Dreamcast)
    symbols.txt    every named address of the arcade's program (symbols_dc.txt: the Dreamcast's)
    pointers.tsv   the words of the programs that hold addresses (pointers_dc.tsv: the Dreamcast's), which the
                   port needs to load the program's data

## License

GNU Affero General Public License v3.0 (`LICENSE`). `NOTICE.md` lists what the license does not cover, and
`ports/NOTICE.md` the libraries the Vita build links (vitaGL and others, with their licenses in
`ports/licenses/`).
