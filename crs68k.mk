# Cho Ren Sha 68k: the game's side of the build, included by the Makefile
# after the SDK's rules (so C_OBJECTS is known).
#
#   make            the homebrew (ChoRenSha.bin) and the SD card's homebrews
#                   folder in build/sd: the .bin and its pack (with the music
#                   and the ver1.10 backgrounds)
#   make sd         the same
#   MUSIC_RATE=Hz   the music's rate in the pack (default 11025: about
#                   8.5 MB for the 26 minutes of music; 16000: 12.3 MB)
#   make pc         the PC version, build/host/crs_sdl (SDL2)
#   make check      the attract demo on the PC, from the image and from the
#                   pack, against the Amiga port's reference run, and the
#                   ver1.10 backgrounds through every stage (bg-check)
#   make bg-check   that last part alone (tests/tools/bg_check.c)
#   make music      game/music again from game/data/BGM_DAT (optional: the
#                   streams are in the repository; see tools/music)
#   make musashi    src/musashi again (optional, see tools/update_musashi.sh)
#
# Host tools: gcc, python3; tools/bin's vasm and vlink are Linux x86-64
# programs (Linux, WSL, or the template's Docker image).

HOSTCC ?= gcc
MUSIC_RATE ?= 11025

GAME_BUILD := build/game
HOST_BUILD := build/host
SD_DIR     := build/sd/homebrews
PACK_NAME  := $(basename $(PACKED_BIN))

GAME_SOURCES := game/sz2.s game/sincos.dat game/mem_map.s game/host_stubs.s tools/build_image.sh
GAME_DATA    := $(shell find game/data -type f)
MUSIC        := $(wildcard game/music/*.crm)

HOST_SOURCES := \
src/crs/crs_core.c \
src/crs/crs_memory.c \
src/crs/crs_render.c \
src/crs/crs_bg.c \
src/crs/crs_audio.c \
src/musashi/m68kcpu.c \
src/musashi/m68kops.c \
src/musashi/softfloat/softfloat.c
HOST_CFLAGS := -O2 -Isrc/crs -Isrc/musashi -I$(GAME_BUILD)

#######################################
# The game image and its symbols
#######################################
# crs_symbols.h (CRS_SYM_*, CRS_IMAGE_ID) comes from the image: the
# homebrew and the pack are always made from the same one.

$(GAME_BUILD)/crs_symbols.h: $(GAME_SOURCES)
	$(V)$(ECHO) [ GAME ] sz2.s
	$(V)tools/build_image.sh $(GAME_BUILD)

$(GAME_BUILD)/sz2_host.bin: $(GAME_BUILD)/crs_symbols.h

$(C_OBJECTS): $(GAME_BUILD)/crs_symbols.h

#######################################
# The pack and the SD card's folder
#######################################

$(HOST_BUILD)/make_crs: tools/make_crs.c $(HOST_SOURCES) src/crs/crs_pack.h src/crs/crs_ima.h $(GAME_BUILD)/crs_symbols.h
	$(V)$(ECHO) [ HOSTCC ] make_crs
	$(V)mkdir -p $(HOST_BUILD)
	$(V)$(HOSTCC) $(HOST_CFLAGS) -o $@ tools/make_crs.c $(HOST_SOURCES) -lm

# MUSIC_RATE, rewritten when it changes so the pack is made again.
$(HOST_BUILD)/music_rate: FORCE
	$(V)mkdir -p $(HOST_BUILD)
	$(V)echo $(MUSIC_RATE) | cmp -s - $@ || echo $(MUSIC_RATE) > $@

# The game once it has loaded its files, its backgrounds and the music.
$(SD_DIR)/$(PACK_NAME).crs: $(HOST_BUILD)/make_crs $(HOST_BUILD)/music_rate $(GAME_BUILD)/sz2_host.bin $(GAME_DATA) $(MUSIC)
	$(V)$(ECHO) [ PACK ] $(PACK_NAME).crs
	$(V)mkdir -p $(SD_DIR) $(HOST_BUILD)/make_crs_work
	$(V)rm -f $(HOST_BUILD)/make_crs_work/*
	$(V)$(HOST_BUILD)/make_crs $(GAME_BUILD)/sz2_host.bin game/data game/music $(HOST_BUILD)/make_crs_work $@ $(MUSIC_RATE)

# The template's pack rule makes $(PACKED_BIN) (no file rule of its own).
$(SD_DIR)/$(PACKED_BIN): pack
	$(V)mkdir -p $(SD_DIR)
	$(V)cp $(PACKED_BIN) $@

.PHONY: sd FORCE
sd: $(SD_DIR)/$(PACKED_BIN) $(SD_DIR)/$(PACK_NAME).crs
	$(V)rm -rf $(SD_DIR)/$(PACK_NAME)
	$(V)$(ECHO) "[ SD ] build/sd: copy its homebrews folder to the SD card's root"

all: sd

#######################################
# PC version, checks
#######################################
.PHONY: pc check bg-check

pc: $(HOST_BUILD)/crs_sdl

$(HOST_BUILD)/crs_sdl: src/pc/sdl_main.c $(HOST_SOURCES) $(GAME_BUILD)/crs_symbols.h
	$(V)$(ECHO) [ HOSTCC ] crs_sdl
	$(V)mkdir -p $(HOST_BUILD)
	$(V)$(HOSTCC) $(HOST_CFLAGS) $$(sdl2-config --cflags) -o $@ src/pc/sdl_main.c $(HOST_SOURCES) \
		$$(sdl2-config --libs) -lm

check: pc $(SD_DIR)/$(PACK_NAME).crs bg-check
	$(V)tools/check.sh $(HOST_BUILD)/crs_sdl $(SD_DIR)/$(PACK_NAME).crs

# The ver1.10 backgrounds: the attract demo, then a game through all the
# stages (with cheats); no metatile may be drawn on screen.
$(HOST_BUILD)/bg_check: tests/tools/bg_check.c $(HOST_SOURCES) $(GAME_BUILD)/crs_symbols.h
	$(V)$(ECHO) [ HOSTCC ] bg_check
	$(V)mkdir -p $(HOST_BUILD)
	$(V)$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/tools/bg_check.c $(filter-out src/crs/crs_bg.c,$(HOST_SOURCES)) -lm

bg-check: $(HOST_BUILD)/bg_check $(GAME_BUILD)/sz2_host.bin
	$(V)rm -rf build/bg_check && mkdir -p build/bg_check
	$(V)$(HOST_BUILD)/bg_check 6000 > build/bg_check/demo.txt || { cat build/bg_check/demo.txt; exit 1; }
	$(V)tail -n 1 build/bg_check/demo.txt
	$(V)$(HOST_BUILD)/bg_check 100000 --play > build/bg_check/play.txt || { cat build/bg_check/play.txt; exit 1; }
	$(V)tail -n 1 build/bg_check/play.txt

#######################################
# Made again on request
#######################################
.PHONY: music musashi

music:
	$(V)tools/music/build_music.sh

musashi:
	$(V)tools/update_musashi.sh

clean::
	$(V)rm -rf $(GAME_BUILD) $(HOST_BUILD) build/sd build/check
