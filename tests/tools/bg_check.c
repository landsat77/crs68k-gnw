/*
 * bg_check: runs the game headless and checks the ver1.10 background engine
 * (src/crs/crs_bg.c, included here): it must never change a metatile on
 * the visible part of GVRAM (every ring row it draws must be off the 256
 * lines shown, before and after the frame's scroll). Prints the loops it
 * goes through.
 *
 * usage: bg_check [frames [shots_dir]] [--play]
 *   --play   start a game from the title and play it with cheats (the ship
 *            always just respawned, so invincible; the stage scripts never
 *            wait for a boss) to go through every stage
 *   shots_dir: the screen every 600 frames (BMP, 24-bit)
 *
 * Environment (for looking closer): NO_CHEATS=1 (--play without cheats),
 * BG100=1 (draw the ver1.0x backgrounds), LAYER=0|1 (shots of one
 * background layer alone, transparent = green), SHOT_FROM, SHOT_TO,
 * SHOT_EVERY (more shots, in frames). Run from the repository's root after
 * `make` (it reads build/game/sz2_host.bin and game/data).
 *
 * Built by `make bg-check` (crs68k.mk).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/crs/crs_bg.c"
#include "crs_symbols.h"

static int visible(int slot, int scroll)
{
	/* GVRAM lines slot * 32 .. + 31 against scroll .. scroll + 255 */
	for (int y = 0; y < 32; y++)
		if (((slot * 32 + y - scroll) & 511) < 256) return 1;
	return 0;
}

static void save_bmp(const char *path)
{
	static uint16_t screen[CRS_SCREEN_WIDTH * CRS_SCREEN_HEIGHT];
	crs_render(screen, CRS_SCREEN_WIDTH, 0, CRS_SCREEN_HEIGHT);
	if (getenv("LAYER")) {	/* one background layer alone */
		int l = atoi(getenv("LAYER"));
		uint8_t line[CRS_SCREEN_WIDTH];
		for (int y = 0; y < CRS_SCREEN_HEIGHT; y++) {
			crs_bg_line(l, (crs_bg_scroll(l) + y) & 511, 0, CRS_SCREEN_WIDTH, line);
			for (int x = 0; x < CRS_SCREEN_WIDTH; x++) {
				uint16_t c = crs_bg_colour(line[x]);
				uint32_t g = (c >> 11) & 31, r = (c >> 6) & 31, b = (c >> 1) & 31;
				screen[y * CRS_SCREEN_WIDTH + x] = line[x] ? (r << 11 | g << 6 | b) : 0x07E0;
			}
		}
	}
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint32_t w = CRS_SCREEN_WIDTH, h = CRS_SCREEN_HEIGHT, size = 54 + w * h * 3;
	uint8_t hd[54] = { 'B', 'M' };
	uint32_t v[] = { size, 0, 54, 40, w, h };
	for (int i = 0; i < 6; i++) memcpy(hd + 2 + i * 4, &v[i], 4);
	hd[26] = 1; hd[28] = 24;
	fwrite(hd, 1, 54, f);
	for (int y = h - 1; y >= 0; y--)
		for (uint32_t x = 0; x < w; x++) {
			uint16_t c = screen[y * w + x];
			uint8_t px[3] = { (c & 31) << 3, ((c >> 5) & 63) << 2, (c >> 11) << 3 };
			fwrite(px, 1, 3, f);
		}
	fclose(f);
}

int main(int argc, char **argv)
{
	int frames = 6000, play = 0;
	const char *shots = NULL;
	for (int i = 1, n = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--play")) play = 1;
		else if (n++ == 0) frames = atoi(argv[i]);
		else shots = argv[i];
	}
	crs_config_t config = { .image = "build/game/sz2_host.bin", .data_dir = "game/data",
	                        .save_dir = "build/bg_check", .music = 0, .quiet = 1 };
	if (crs_init(&config)) return 1;
	if (getenv("BG100")) crs_bg_enabled = 0;
	int errors = 0, changes = 0, last_end = 0, last_stage = -1;
	uint8_t before[2][RING_ROWS][COLUMNS];
	for (int f = 0; f < frames; f++) {
		if (play) {
			crs_input.autofire = 1;
			crs_input.joystick = (f >= 200 && f < 210) || (bg.active && f % 2 && !getenv("NO_CHEATS")) ? CRS_JOY_A : 0;
			if (bg.active && !getenv("NO_CHEATS")) {
				uint32_t respawn = crs_rd16(CRS_SYM_WORD_000A8EFC);
				if (respawn < 0x40) crs_wr16(CRS_SYM_WORD_000A8EFC, 0x40);
				crs_wr16(CRS_SYM_WORD_0008DB2C, 0);
			}
		}
		int shown[2] = { crs_bg_scroll(0), crs_bg_scroll(1) }, was_active = bg.active;
		memcpy(before, bg.ring, sizeof before);
		int builds = crs_graphics_builds;
		if (crs_run_frame() == CRS_EXITED) break;
		if (crs_graphics_builds != builds) was_active = 0;	/* a game starts: the whole ring */
		for (int l = 0; l < 2; l++)
			for (int r = 0; r < RING_ROWS; r++)
				for (int c = 0; c < COLUMNS; c++) {
					if (before[l][r][c] == bg.ring[l][r][c]) continue;
					changes++;
					int slot = (r - SLOT_OFFSET) & 15;
					if (was_active && bg.ring[l][r][c] && (visible(slot, shown[l]) || visible(slot, crs_bg_scroll(l)))) {
						if (errors++ < 20)
							printf("frame %d: layer %d ring row %d column %d changed on screen\n", f, l, r, c);
					}
				}
		int stage = crs_rd16(CRS_SYM_PLAYER_SCORE + 4);
		if (bg.end[0] != last_end || (bg.active && stage != last_stage)) {
			printf("frame %d: stage %x phase %03x time %03x: loop rows %d to %d, speeds %d %d\n", f, stage,
			       crs_rd16(CRS_SYM_WORD_000A8EEA), crs_rd16(CRS_SYM_WORD_000A8EEC), bg.end[0], bg.start[0] - 1,
			       bg.target[0], bg.target[1]);
			last_end = bg.end[0];
			last_stage = stage;
		}
		if (shots && (f % 600 == 0 || (getenv("SHOT_FROM") && f >= atoi(getenv("SHOT_FROM")) && f < atoi(getenv("SHOT_TO")) && f % atoi(getenv("SHOT_EVERY")) == 0))) {
			char path[1024];
			snprintf(path, sizeof path, "%s/bg_%06d.bmp", shots, f);
			save_bmp(path);
		}
	}
	printf("bg_check: %d metatiles drawn, %d on screen\n", changes, errors);
	return errors ? 1 : 0;
}
