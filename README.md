# Cho Ren Sha 68k for Game & Watch Retro-Go SD

<img width="2880" height="2160" alt="image" src="https://github.com/user-attachments/assets/0bd0d491-a01f-447c-b54d-c1de5015ad5e" />




**Cho Ren Sha 68k** (超連射68k), the X68000 vertical shoot 'em up by
Famibe No Yosshin (1995-1998), on the Nintendo Game & Watch (Zelda or
Mario) with the [Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd)
firmware, as a **GWHB homebrew**: a `.bin` and its pack dropped in
`/homebrews/`, no firmware rebuild needed.

The game's own 68000 program runs in Musashi, and a port layer in C does
what the X68000 hardware and Human68k did (screen, sound, files, timing).
Since v1.1.0 the stages have the **new backgrounds of ver1.10** (2023-2025):
long maps of metatiles in two layers, a palette per stage, scroll speeds
and transitions driven by the stage scripts, as in the ver1.10 X68000
program (see [Backgrounds](#the-ver110-backgrounds)). The game itself is
ver1.0x's, which ver1.10 keeps (its replays play back on both).

The project is built from
[retro-go-sd-templates](https://github.com/sylverb/retro-go-sd-templates)
and talks to the firmware only through the firmware ABI. Everything is in
this repository: the game's source and data (Cho Ren Sha 68k is free
software from its author), the tools, the homebrew, and a PC version to
check it.

## Installation

Requires Retro-Go SD **v2.0.0-rc1 or later** (firmware ABI v2).

1. Download `ChoRenSha-vX.Y.Z.zip` from the
   [releases](https://github.com/landsat77/crs68k-gnw/releases) (or build
   it, below).
2. Copy its `homebrews` folder to the SD card's root:
   ```
   /homebrews/ChoRenSha.bin     the homebrew (Homebrew tab)
   /homebrews/ChoRenSha.crs     the game, its music and backgrounds, same name
   ```
3. Start "Cho Ren Sha 68k" from the **Homebrew** tab. The first start
   copies the pack (about 10 MB) to the external flash ("Caching game"); it
   then runs from there, the SD card is not read during the game.

High scores and settings the game writes go to `/retro-go/saves/crs68k/`.
See [INSTALL.md](INSTALL.md) (English and French).

## Controls

| Game & Watch | Cho Ren Sha 68k |
|---|---|
| D-pad | Move, menus |
| A | Shot (auto fire on by default) |
| B | Bomb (cancel in menus) |
| GAME (START on Zelda units) | Pause (the X68000's ESC) |
| PAUSE/SET | Retro-Go menu |

The Retro-Go menu has save states and the options **Auto fire**, **Music**
and **Backgrounds** (`1.10`: the new ones, default; `1.0x`: the original
game's single tiled picture).

## The ver1.10 backgrounds

Ver1.10 was made from salvaged ver1.1x X68000 files (released in 2023 for
the X68000 Z, then for Windows in 2025). Its backgrounds are two maps of
8 x 1024 metatiles of 32 x 32 pixels (`map.bg0`, `map.bg1`, `map.chp`,
`bg_pcg.sp`) shared by all the stages, and 9 palettes. The engine draws
them into the X68000's two 512 x 512 graphics pages as a ring of 16 rows,
loops on 16 rows of the map and goes through transition bands to the next
loop, at speeds that change during the stages (sometimes backwards for the
back layer), and fades the palette from a stage's to the next one's.

`src/crs/crs_bg.c` does in C what the ver1.10 X68000 program (`SZ2.X` on
the official `CH68_110.xdf`) does for them, from its disassembly: the
scroll and drawing of the rows (`$56AE`), the loops (`$58DE`, `$5938`), the
palette fades (`$479A`, `$4864`), the colour cycling of the VBL, and a
table of the stage scripts' background actions (stage, phase, time). The
ver1.0x game calls it at the same points: a game's start, each run of a
stage script, each game frame, each VBL (`game/sz2.s` and
`game/host_stubs.s`, `__GNW__`). GVRAM is not kept in RAM: the renderer
draws its lines from the metatile numbers the engine drew in the ring.

One change from ver1.10: when the back layer has scrolled backwards (some
bosses), ver1.10 draws the next rows where the last ones are, which can
then be on screen; here the drawing waits until the screen is back there,
so no row ever appears on screen. `make bg-check` plays every stage (with
cheats) and checks that.

Not taken from ver1.10: its new sprites, sound effects and music
arrangements, and its new ending (stage 7's last phase, `end.chp`).

The data are in `game/data/V110`, from the official Windows archive of
ver1.10 (`crs68k_110_build20250615_wip19.zip`, SHA-256 `ce2a588d...d035`).

## Building

```
make            # the homebrew and its pack, in build/sd/homebrews
make check      # the attract demo against the Amiga port's reference run,
                # and the ver1.10 backgrounds through every stage
make pc         # the PC version: build/host/crs_sdl (keys at the top of src/pc/sdl_main.c)
make clean
```

Needs Linux (or WSL on Windows): `arm-none-eabi-gcc` 10 or newer with
newlib, `make`, `gcc`, `python3` with Pillow; SDL2 (`libsdl2-dev`) for
`make pc` and `make check`. `tools/bin` holds vasm and vlink for Linux
x86-64. On Ubuntu:
`sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi libsdl2-dev python3-pil`.

`make` builds, in this order:

1. **The game image**: `game/sz2.s` assembled for the Game & Watch
   (`__AMIGA__` and `__GNW__`), `game/host_stubs.s` in place of the Amiga
   port layer, `game/mem_map.s`; linked at $10000 (`tools/build_image.sh`).
   Its symbols become `build/game/crs_symbols.h`, with `CRS_IMAGE_ID`, the
   image's checksum.
2. **The homebrew**, `ChoRenSha.bin` (the template's build), which knows
   those symbols.
3. **The pack**, `ChoRenSha.crs` (`tools/make_crs.c`, run on the PC): the
   game as it is once it has loaded `game/data` (its memory, read in place
   from external flash on the device), the ver1.0x backgrounds it builds,
   the ver1.10 backgrounds (`game/data/V110`), and the music
   (`game/music`) as mono IMA ADPCM at `MUSIC_RATE` Hz (default 11025:
   about 8.3 MB of the pack's 10 MB; `make MUSIC_RATE=16000` sounds clearer
   for about 12 MB). The homebrew only takes a pack made from the same
   image, which one `make` guarantees.
4. **The SD card's folder**, `build/sd/homebrews`.

Releases: a release made on github.com (tag `vX.Y.Z`, with its section in
`CHANGELOG.md`) makes the CI build, check, and attach
`ChoRenSha-vX.Y.Z.zip` (the SD card's files and `INSTALL.md`) and a debug
zip (ELF, map).

## Layout

| Path | What |
|---|---|
| `Makefile` | The template's, changed only where it must be (kind, name, sources, cover), and `include crs68k.mk` |
| `crs68k.mk` | The game's rules: image, pack, SD folder, PC version, checks, music, Musashi |
| `src/main.c` | The homebrew: pack in flash, RAM pool, picture, sound, buttons, menu options, save states |
| `src/crs/` | The portable port layer: memory in 256-byte pages, Human68k/IOCS, frames, renderer, sound, save states |
| `src/crs/crs_bg.c` | The ver1.10 backgrounds (engine, stage script actions, drawing) |
| `src/musashi/` | Musashi (MIT), generated and slimmed for the device (`make musashi` makes it again) |
| `src/pc/sdl_main.c` | The PC version (SDL2): plays, and runs checks headless (input scripts, save states, RAM log) |
| `src/assets/` | The launcher cover (`cover.png`, made into a JPEG by `scripts/make_cover.py`), logos (core kind) |
| `game/sz2.s` | The game, a re-assemblable disassembly of the X68000 program, with the Atari, Amiga and Game & Watch ports' changes |
| `game/host_stubs.s`, `game/mem_map.s`, `game/sincos.dat` | The rest of the game image |
| `game/data/` | The game's files (BGM_DAT, ETC_DAT, PCM_DAT, XSP_DAT), and `V110`: ver1.10's backgrounds |
| `game/music/` | The songs rendered from `game/data/BGM_DAT` (`make music` renders them again, see `tools/music`) |
| `tools/` | vasm and vlink, `build_image.sh`, `make_crs.c`, `check.sh`, `checkpoints.py`, `slim_musashi.py`, `update_musashi.sh`, `music/` |
| `tests/` | The Amiga port's reference run of the attract demo, input scripts for `crs_sdl --input`, `tools/bg_check.c` |
| `sdk/`, `scripts/`, `host/` | From the template |
| `.github/workflows/ci.yml` | Build and checks on every push; release files on a tag |

## What differs from the game on the X68000

- The game's CONFIG menu leaves out SCREEN, XSP_VERTICAL and STK_VERTICAL
  (one LCD mode, no rotated screen; their values stay the defaults).
- The picture is the middle 240 of the X68000's 256 lines.
- Each press of START reaches the game as one clean press (300 ms before
  the next): on the device the pause otherwise ended at once.

## Memory

From the pack, the attract demo needs at most 136 KB of RAM for the pages
the game writes; the rest of RAM_EMU after the 213 KB `.bin` holds them.
Save states hold the pages that differ from the pack, about 120 KB, and
the backgrounds' state (under 1 KB).

## Not done yet

- On the device: speed, sound and timing (the game runs at 55.46 Hz) are
  to be checked; the pause, after the change above.
- Saved high scores and settings are written to `/retro-go/saves/crs68k`
  but the pack's are used at start-up.

## Licences

The template's SDK: MIT (`LICENSE`). Musashi: MIT
(`src/musashi/README-Musashi.txt`); SoftFloat: its own licence
(`src/musashi/softfloat/README.txt`). vasm and vlink: by Volker Barthelmann
and Frank Wille, free for non-commercial use. MCDRV (downloaded by `make
music`): CUL, free software. Cho Ren Sha 68k, its program and its data
(ver1.0x and ver1.10) belong to Famibe No Yosshin
([official page](https://yosshin4004.github.io/x68k/crs68k/official/index.html)).
