# crs68k: Cho Ren Sha 68k for Game & Watch Retro-Go SD

A [Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd)
homebrew that plays **Cho Ren Sha 68k**, the X68000 vertical shoot 'em up
(c) 1995 Famibe No Yosshin, on the Game & Watch (Zelda or Mario) with the SD
card mod. Built from
[retro-go-sd-templates](https://github.com/sylverb/retro-go-sd-templates)
(its history is kept; the `template` remote points to it), with
`PROJECT_KIND ?= homebrew`.

The game's own 68000 program runs in Musashi, and a port layer in C does
what the X68000 hardware and Human68k did (screen, sound, files, timing).
Everything is in this folder: the game's source and data, the tools, the
homebrew, and a PC version to check it.

## Building

```
make            # the homebrew and the SD card's folder, in build/sd/homebrew
make check      # the attract demo on the PC against the Amiga port's reference run
make pc         # the PC version: build/host/crs_sdl (keys at the top of src/pc/sdl_main.c)
make clean
```

Needs (Linux, WSL on Windows, or the template's Docker image for the
homebrew): `arm-none-eabi-gcc` 10 or newer, `make`, `gcc`, `python3` with
Pillow; SDL2 (`libsdl2-dev`) for `make pc` and `make check`.
`tools/bin` holds vasm and vlink for Linux x86-64.

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
   from external flash on the device) and the backgrounds it builds. The
   homebrew only takes a pack made from the same image, which one `make`
   guarantees. The music (`game/music`) goes in it too, as mono IMA ADPCM
   at `MUSIC_RATE` Hz (default 11025: about 8.3 MB of the pack's 10 MB;
   `make MUSIC_RATE=16000` sounds clearer for about 12 MB), so it plays
   from flash and the SD card is not read while the game runs.
4. **The SD card's folder**, `build/sd/homebrew`.

## SD card

Copy `build/sd/homebrew` to the card's root:

```
/homebrew/ChoRenSha.bin           the homebrew (the Homebrew tab)
/homebrew/ChoRenSha.crs           the game and its music, beside it, same name
/retro-go/saves/crs68k/           high scores and settings the game writes
```

The firmware copies the pack to the external flash
(`odroid_overlay_cache_file_in_flash`), so the flash needs room for it:
about 10 MB at the default `MUSIC_RATE`. A `/homebrew/ChoRenSha/MUSIC`
folder from an earlier build is no longer used and can be deleted.

Firmware: Retro-Go SD with firmware ABI version 2.

Buttons: D-pad, A = shot, B = second button, START or GAME = the game's
pause (the X68000's ESC). Firmware menu (PAUSE/SET): save states, and the
options auto fire and music.

## Layout

| Path | What |
|---|---|
| `Makefile` | The template's, changed only where it must be (kind, name, sources), and `include crs68k.mk` |
| `crs68k.mk` | The game's rules: image, pack, SD folder, PC version, check, music, Musashi |
| `src/main.c` | The homebrew: pack in flash, RAM pool, picture, sound, buttons, menu options, save states |
| `src/crs/` | The portable port layer: memory in 256-byte pages, Human68k/IOCS, frames, renderer, sound, save states |
| `src/musashi/` | Musashi (MIT), generated and slimmed for the device (`make musashi` makes it again) |
| `src/pc/sdl_main.c` | The PC version (SDL2): plays, and runs checks headless (input scripts, save states, RAM log) |
| `src/assets/` | Launcher logos (for the core kind) |
| `game/sz2.s` | The game, a re-assemblable disassembly of the X68000 program, with the Atari, Amiga and Game & Watch ports' changes |
| `game/host_stubs.s`, `game/mem_map.s`, `game/sincos.dat` | The rest of the game image |
| `game/data/` | The game's files (BGM_DAT, ETC_DAT, PCM_DAT, XSP_DAT) |
| `game/music/` | The songs rendered from `game/data/BGM_DAT` (`make music` renders them again, see `tools/music`) |
| `tools/` | vasm and vlink, `build_image.sh`, `make_crs.c`, `check.sh`, `checkpoints.py`, `slim_musashi.py`, `update_musashi.sh`, `music/` |
| `tests/` | The Amiga port's reference run of the attract demo, input scripts for `crs_sdl --input` |
| `sdk/`, `scripts/`, `host/`, `.github/` | From the template |

## What differs from the game on the X68000

- The game's CONFIG menu leaves out SCREEN, XSP_VERTICAL and STK_VERTICAL
  (one LCD mode, no rotated screen; their values stay the defaults).
- The picture is the middle 240 of the X68000's 256 lines.
- Each press of START reaches the game as one clean press (300 ms before
  the next): on the device the pause otherwise ended at once.

## Memory

From the pack, the attract demo needs at most 136 KB of RAM for the pages
the game writes; the rest of RAM_EMU after the 210 KB `.bin` holds them.
Save states hold the pages that differ from the pack, about 120 KB.

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
belong to their authors.
