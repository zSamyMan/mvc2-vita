# The PS Vita port

Marvel vs. Capcom 2's Dreamcast version on the PS Vita, built on the decompilation in `../src` and
`../include`. It is not an emulator: the decompiled game is compiled for the Vita, and the port supplies what
the Dreamcast and Sega's libraries did.

**No game data is included.** Put the USA disc's files (`1ST_READ.BIN` and the rest) in
`ux0:data/mvc_2/disc`, or any one of these, which the first start unpacks into that folder (210 MB, once; the
image can be deleted after):

| What                                   | Example                                                 |
|----------------------------------------|---------------------------------------------------------|
| a CHD                                  | `Marvel vs. Capcom 2 (USA).chd`                         |
| a GDI with its tracks                  | `disc.gdi`, `track01.bin`, `track02.raw`, `track03.bin` |
| a CUE with its BINs (Redump's set)     | `Marvel vs. Capcom 2 (USA).cue` and its three `.bin`s   |
| track 3 alone, raw or as an ISO        | `Marvel vs. Capcom 2 (USA) (Track 3).bin`, `mvc2.iso`   |

Everything else the port keeps is in `ux0:data/mvc_2/` too: `mvc2.cfg`, the memory card's saves, the shaders it
compiles (`shaders/pack.bin`) and, for testing, `env.txt` (`NAME=value` lines read as environment variables;
`DEBUG_OPTIONS=0` turns off the testing tools: the overlay, the pictures, the log and their buttons).

Sound: the AICA's own driver runs on an emulated ARM7 (translated to the Vita's own ARM code) with its DSP; the
ADX music is decoded by the port.

## How it fits together

- `CMakeLists.txt` compiles the decompilation's C as it is (`MVC2_DECOMP`) for the Dreamcast (`TARGET_DC`,
  with `src/dc`), with the options in `port/include/port_prefix.h` standing in for SHC's built-ins. The
  program becomes one relocatable object whose only global name is its program's, linked over one host.
  `port/tools/vita_rewrite.py` moves its main RAM addresses to the Vita's mirror of it.
- `port/tools/gen_host.py` finds what a program uses that nothing defines: its data left in place (filled
  from the user's program image at start-up and relocated), fixed buffers, and a stub for each library
  routine the port lacks yet.
- `port/naomi/` is the board: main RAM, the game run as a coroutine, its interrupts, and Sega's library
  routines as the port implements them.
- `port/dc/` is what the Dreamcast adds: its program from `1ST_READ.BIN`, the disc (gdFs) over a folder,
  Shinobi's calls, the memory card and the ADX music's decoder.
- `port/snd/` is the sound: the AICA's ARM7 and DSP, and Sega's sound libraries' calls over them.
- `port/render/rd_vgl.c` draws the game's frames with vitaGL on its own thread; `port/vita/` is the Vita's
  system side (main RAM through kubridge, the clocks, the log); `port/sdl/` the front end over SDL2;
  `port/sys/` runs the game as a coroutine and makes a division by zero give 0, as SHC's code did.

## Building (VitaSDK, vitaGL)

The Dreamcast version, on a PS Vita or PS TV with HENkaku/Enso, taiHEN, kubridge and the shader compiler
(`libshacccg.suprx` in `ur0:data/`). With `VITASDK` set and vitaGL, vitaShaRK, SceShaccCgExt, libmathneon
and SDL2 installed (`vdpm vitaGL vitaShaRK SceShaccCgExt libmathneon sdl2`):

    cmake -G Ninja -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
    ninja -C build-vita

That makes `mvc2dc.vpk`, installed with VitaShell. The game's files go in `ux0:data/mvc_2/disc`.

### Rebuilding with a modified vitaGL

vitaGL (LGPL-3.0) and the other libraries in `NOTICE.md`'s PS Vita section are linked statically. To run the
port on a changed copy of one of them, build and install that copy into the SDK, then rebuild and reinstall the
port as above:

    git clone https://github.com/Rinnegatamante/vitaGL && cd vitaGL
    git checkout 2bdbe89                   # the release's vitaGL; then your changes
    make NO_DEBUG=1 && make install        # as the vdpm package builds it, into $VITASDK/arm-vita-eabi
    ninja -C build-vita                    # relinks mvc2dc.vpk against it

The new VPK installs over the old one with VitaShell, as any homebrew does: the Vita asks for no key or
signature beyond what the build makes.

