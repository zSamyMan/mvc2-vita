# Marvel vs. Capcom 2 for the PS Vita

**AI Disclosure**:
This project is AI assisted and heavily relied on Claude for the rendering pipeline/VitaGL, with that said, i myself also wrote portions
of the code, and did some optimizing work with the limited knowledge i have about C and the hardware itself. No Gen AI
was used for assets.

A native PS Vita port of *Marvel vs. Capcom 2: New Age of Heroes* (Dreamcast/NAOMI version). It is not
an emulator: the game's code is natively compiled for the Vita's ARM CPU, and the port supplies what the
Dreamcast and Sega's libraries did, with vitaGL drawing on the Vita's GPU.

**No game data is included.** The game's program, graphics and sound come from your own copy of the USA
Dreamcast disc, read at start-up.

## Disclaimer

The game code is under `src/` and `include/` contains the decompilation of the NAOMI arcade and Dreamcast releases by
Giblet (`mvc2-naomi-dc`), which rebuilds both programs byte for byte. This
repository keeps only what the Vita port needs from it, and does not aim to contribute back to the original project: the
decompilation's own toolchain, tests and documentation live in there.

## Installing and running

1. A PS Vita or PS TV with HENkaku, [kubridge](https://github.com/bythos14/kubridge) and [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D/releases).
2. Download `mvc2dc.vpk` from the Releases page and install `mvc2dc.vpk` with VitaShell.
3. Put the USA image of the disc (`.chd`, `.gdi`, `.cue`,
   or an `.iso` of track 3), in `ux0:data/mvc_2/disc/`. An image is unpacked into that folder at the first start. (Takes about 2 minutes)

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

## Credits
- [Rinnegatamante](https://github.com/Rinnegatamante) for VitaGL and VitaShaRK/ShaRKBR33D
- [Giblet](https://github.com/gibletto) for the original MvC2 decompilation and 3DS port which this version is based of
- The [VitaSDK](https://vitasdk.org/) team

## License

The Vita's port code and the changes made for the console are under GNU Affero General Public License v3.0 (`LICENSE`).
`ports/NOTICE.md` show the libraries the Vita build links (VitaGL and others, with their respective licenses in
`ports/licenses/`).

The game's code comes from the decompilation, and the LiveArea assets use the game's artwork, belonging to their rights holders.
This project is not affiliated with or endorsed by MARVEL/CAPCOM/DISNEY, and does NOT distribute any of the game's data. Provide
your own legally obtained copy in order to use it.
