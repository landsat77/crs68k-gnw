#!/bin/bash
#
# `make check`: the game's 6,000-frame attract demo on the PC (headless),
# started from the image and from the pack. Both must match the Amiga
# port's reference run (tests/reference/attract-6000.txt: score, random
# table index, scroll, sprites, player every 60 frames), and the pack's
# screens every 500 frames must be byte for byte the image's.
#
# usage: tools/check.sh crs_sdl pack.crs

set -e

sdl=$1
pack=$2
check=build/check
rm -rf "$check"
mkdir -p "$check/image/save" "$check/image/shots" "$check/pack/save" "$check/pack/shots"

"$sdl" --frames 6000 --save "$check/image/save" --checkpoints "$check/image/checkpoints.txt" \
	--shots "$check/image/shots"
"$sdl" --pack "$pack" --frames 6000 --save "$check/pack/save" --checkpoints "$check/pack/checkpoints.txt" \
	--shots "$check/pack/shots"

python3 tools/checkpoints.py compare tests/reference/attract-6000.txt \
	"$check/image/checkpoints.txt" "$check/pack/checkpoints.txt"
for shot in "$check"/image/shots/*.bmp; do
	cmp -s "$shot" "$check/pack/shots/$(basename "$shot")" || { echo "pack: $(basename "$shot") differs"; exit 1; }
done
echo "check: image and pack match the reference; $(ls "$check"/image/shots | wc -l) screens identical"
