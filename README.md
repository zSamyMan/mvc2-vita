# Marvel vs. Capcom 2 for the PS Vita

**AI Disclosure**:
AI-assisted development, i'm a C developer and i'm familiar with programming for embedded systems, with that said, i still have limited knowledge about the Vita hardware and i absolutely suck at 3D rendering, so AI was used to bridge that gap on my skillset. NO GEN AI was used in any of the artwork/assets, some of them are ripped straight from other releases of the game/the game itself and others are human-made by myself to fit the overall aesthetic

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

### Unpacking on a computer instead (optional)

The disc's files can be unpacked on a PC and copied to the Vita, in place of step 3: nothing is unpacked on the
console then, and the image never has to go onto the memory card. Download the unpacker for your system from the
Releases page (`mvc2_extract-windows-x86_64.exe` or `mvc2_extract-linux-x86_64`), then:

1. Unpack the image:
   - **Windows:** drag the image file (`.chd`, `.gdi`, `.cue` or `.iso`) onto `mvc2_extract-windows-x86_64.exe`,
     or from a command prompt: `mvc2_extract-windows-x86_64.exe "C:\path\to\image.chd"`
   - **Linux:** `chmod +x mvc2_extract-linux-x86_64` then `./mvc2_extract-linux-x86_64 /path/to/image.chd`

   The files go into a `disc` folder beside the image (or a folder given as a second argument). The unpacker
   checks them against the USA disc and says so if anything is missing.
2. Copy **everything** in that `disc` folder, its 32 subfolders included, to `ux0:data/mvc_2/disc/` on the Vita
   using VitaShell.
3. Start the game: it finds the files already unpacked and goes straight to the settings menu.

Building the unpacker yourself? see `ports/tools/extract/CMakeLists.txt` (CMake; for Windows from Linux, with
mingw-w64 and `ports/tools/extract/mingw64.cmake`).

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
