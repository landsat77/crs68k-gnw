#!/bin/bash
#
# The game image the homebrew and its pack are made from (crs68k.mk runs
# it): game/sz2.s assembled as for the Amiga port with __GNW__, the host
# stubs (game/host_stubs.s) in place of the Amiga port layer and the X68000
# memory map (game/mem_map.s: absolute addresses, plain memory for the port
# layer), linked at $10000 with tools/bin's vasm and vlink.
#
# Makes in out_dir: sz2_host.bin, sz2_host.map, and crs_symbols.h (the
# image's global symbols, CRS_SYM_name, and CRS_IMAGE_ID, its checksum:
# the homebrew only takes packs made from the same image).
#
# usage: tools/build_image.sh out_dir

set -e

mkdir -p "$1"
out=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")/.." && pwd)
vasm=$here/tools/bin/vasmm68k_mot
vlink=$here/tools/bin/vlink
mkdir -p "$out"

(
	cd "$here/game"
	"$vasm" sz2.s -quiet -Felf -no-opt -m68030 -D__AMIGA__ -D__GNW__ -o "$out/sz2_host.o"
	"$vasm" mem_map.s -quiet -Felf -no-opt -m68000 -o "$out/mem_map.o"
	"$vasm" host_stubs.s -quiet -Felf -no-opt -m68030 -o "$out/host_stubs.o"
)
"$vlink" -brawbin1 -Ttext 0x10000 -M -o "$out/sz2_host.bin" \
	"$out/sz2_host.o" "$out/mem_map.o" "$out/host_stubs.o" > "$out/sz2_host.map"

# "  NAME: global reloc, value 0x121d0, size 0" -> #define CRS_SYM_NAME 0x121d0u
{
	echo "/* Made by tools/build_image.sh from the link map: the game's global symbols. */"
	echo "#ifndef CRS_SYMBOLS_H"
	echo "#define CRS_SYMBOLS_H"
	sed -n 's/^  \([A-Za-z_][A-Za-z0-9_]*\): global reloc, value \(0x[0-9a-fA-F]*\),.*/#define CRS_SYM_\1 \2u/p' "$out/sz2_host.map" | sort -u
	echo "#define CRS_IMAGE_ID $(cksum < "$out/sz2_host.bin" | cut -d' ' -f1)u"
	echo "#endif"
} > "$out/crs_symbols.h"
