# Changelog

This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Release tags must
match a section heading exactly (for example `v0.1.0`): CI uses that section
as the GitHub Release notes.

## [Unreleased]

## [v1.1.0] - 2026-10-25

### Added

- The backgrounds of ver1.10: its two maps of metatiles, its palettes
  (one per stage, faded from one to the next) and its scroll engine (speeds
  per layer that change during the stages, loops of 16 rows, transitions),
  driven by the stage scripts as in the ver1.10 X68000 program
  (`src/crs/crs_bg.c`, data in `game/data/V110`, pack section "BG11").
  Save states keep them.
- Option **Backgrounds** in the Retro-Go menu (and F2 / `--bg100` on the
  PC): `1.10` (default) or `1.0x`, the original game's.
- `make bg-check` (part of `make check`): the attract demo and a game
  through every stage (with cheats), checking that no background metatile
  is ever drawn on screen.
- A cover for the launcher (`src/assets/cover.png`, `scripts/make_cover.py`)
  instead of the generated title card.
- CI (`.github/workflows/ci.yml`): build and checks on every push; a
  release made on github.com gets `ChoRenSha-vX.Y.Z.zip` (the SD card's
  files and `INSTALL.md`) and a debug zip.
- `INSTALL.md` (English and French).

### Fixed

- The SD card's folder is `/homebrews/` (the firmware's), not `/homebrew/`:
  `make` puts the files in `build/sd/homebrews`.
- The tools (vasm, vlink, the build and check scripts) lost their
  executable bit in the repository: `make` failed with "Permission denied".

## [v1.0.0] - 2026-10-24

### Added

- The Cho Ren Sha 68k homebrew, from retro-go-sd-templates: the game's
  68000 program in Musashi (slimmed for the device), the portable port
  layer (paged memory, Human68k/IOCS, frames, renderer, sound), the pack
  `/homebrew/ChoRenSha.crs` cached in flash, menu options (auto fire,
  music).
- `crs_sdl --wav FILE` records the sound of a headless run.
- Save states, and an error screen that shows the RAM in use and goes back
  to the launcher.
- Everything in one folder: the game's source and data (`game/`), the
  tools (`tools/`), and `crs68k.mk` so that `make` builds the homebrew, the
  pack and the SD card's folder; `make check` and `make pc` for the PC
  version.

### Changed

- The game's CONFIG menu leaves out SCREEN, XSP_VERTICAL and STK_VERTICAL
  (on the first device test, changing SCREEN ended in "out of RAM pages"
  with no way out but removing the battery).
- START (the game's pause): each press of the button reaches the game as
  one clean pulse, with 300 ms before the next (the pause ended at once on
  the device).
- The music is in the pack, as mono IMA ADPCM at `MUSIC_RATE` (default
  11025 Hz, 8.3 MB instead of 65 MB of .crm files), and plays from the
  external flash: streaming it from the SD card glitched on the device.
  It is interpolated to the output rate, and its position no longer drifts
  (the step was rounded to 1/65536 of a sample).

### Fixed

- The template's `[ COVER ]` message broke the build under `/bin/sh`
  (unquoted parentheses).
