/*
 * The backgrounds of Cho Ren Sha 68k ver1.10, on the ver1.0x game.
 *
 * Ver1.10 (2023-2025, from salvaged ver1.1x X68000 files) has new
 * backgrounds: two long maps of 32 x 32 metatiles shared by the stages,
 * 9 palettes, and an engine that scrolls them with per-layer speeds,
 * loops on 16 metatile rows and moves through transition bands, all driven
 * by the stage scripts. The gameplay is the same as ver1.0x (its replays
 * play back on it), so this file does in C what the ver1.10 X68000 program
 * (CH68_110.xdf, SZ2.X) does for its backgrounds, and runs it from the
 * ver1.0x game in Musashi at the same points:
 *
 * - crs_bg_game_start: the start of a game (PLAY_GAME, $38A2 in SZ2.X):
 *   black palette, scroll reset ($5998), loop of the starting stage ($5906)
 *   and the whole ring drawn ($5574);
 * - crs_bg_script: each run of the stage script (STAGE_n), with the
 *   stage, phase and time it starts with: the ver1.10 scripts' background
 *   actions (loops $58DE/$5938, scroll speeds, palette fades $479A/$4864),
 *   from the table below;
 * - crs_bg_frame: once per game frame, where the game counts its
 *   background frames (the scroll and the rows to draw, $56AE);
 * - crs_bg_vbl: each VBL (the colour cycling of the VBL handler, $5A3A);
 * - crs_bg_wipe: IOCS _WIPE (the menus after a game clear GVRAM).
 *
 * GVRAM is not kept: the ring the engine draws its metatiles into (16 rows
 * of 8, 512 x 512 pixels per layer) is held as the metatile numbers it
 * drew (the engine's own cache, $90262), and the renderer draws GVRAM's
 * lines from them (crs_bg_line).
 *
 * Addresses in the comments are SZ2.X's (text at 0).
 */

#include <string.h>
#include "crs.h"

#define ROWS 1024		/* map rows of 8 metatiles (32 x 32 pixels) */
#define COLUMNS 8
#define RING_ROWS 16		/* GVRAM's 512 lines */
#define SLOT_OFFSET 2		/* ring row n is drawn at GVRAM row n - 2 (the upright screen) */

/* Palettes in the data, in SZ2.X's order ($3D558). */
enum { PAL_FB_NW, PAL_FY_NP, PAL_FY_NR, PAL_FB_NP, PAL_FR_NB, PAL_FB_NY, PAL_ST0, PAL_DARK, PAL_LITE };

/* What the engine keeps (saved field by field: crs_bg_state_save). */
typedef struct {
	int32_t pos[2];		/* $5933A: scroll, 1/256 pixel (page 0, page 1) */
	int32_t vel[2];		/* $59332: speed per frame */
	int32_t target[2];	/* $5932A: the speed it moves towards, by 1 a frame */
	int32_t shown[2];	/* the scroll on screen (the frame's snapshot, $3D44) */
	int32_t cycle, cycle_step;	/* $48990, $4898C: colour cycling */
	int16_t line[2];	/* $90466: the scroll line the drawing has reached */
	int16_t fill[2];	/* $9025E: the row to draw next, * 32 */
	int16_t end[2];		/* $9046A: the loop's first row */
	int16_t start[2];	/* $90472: the row after its last */
	int16_t pending[2];	/* $9046E: a loop queued by $5938 */
	int16_t next_start[2];	/* $90476 */
	int16_t next_end[2];	/* $9047A */
	int16_t back[2];	/* lines scrolled down since the drawing stopped (not in SZ2.X) */
	int16_t active;		/* a game has started since the reset */
	uint8_t ring[2][RING_ROWS][COLUMNS];	/* $90262: the metatiles drawn */
	uint16_t palette[256];	/* $4A11C: the graphics palette */
} bg_state_t;

static bg_state_t bg;
static crs_bg_data_t data;
int crs_bg_enabled = 1;

/* --- data --- */

static uint16_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

int crs_bg_set_data(const uint8_t *blob, uint32_t size)
{
	if (!blob || size < CRS_BG_DATA_SIZE) {
		memset(&data, 0, sizeof data);
		return -1;
	}
	data.pcg = blob;
	data.chp = blob + 0x2000;
	data.map[0] = blob + 0x4000;
	data.map[1] = blob + 0x6000;
	data.palettes = blob + 0x8000;
	return 0;
}

const uint8_t *crs_bg_blob(void)
{
	return data.pcg;	/* the start of the data */
}

int crs_bg_available(void)
{
	return data.pcg != NULL;
}

static uint16_t palette_entry(int palette, int i)
{
	return be16(data.palettes + palette * 512 + i * 2);
}

/* The metatile at a map row and column ($55A4, $5794): rows wrap at 1024. */
static uint8_t metatile(int layer, int row, int column)
{
	return data.map[layer][((row & (ROWS - 1)) << 3) + (column & (COLUMNS - 1))];
}

/* --- engine --- */

/* $5998, with the start of $38A2: no scroll, the default speeds, black. */
void crs_bg_reset(void)
{
	memset(&bg, 0, sizeof bg);
	bg.cycle = 0x400;
	bg.cycle_step = 0x40;
	bg.vel[0] = bg.target[0] = -0x400;
	bg.vel[1] = bg.target[1] = -0x100;
}

/* $58DE: the loop is rows a * -16 - b to a * -16 - 1 at once; the drawing
   goes on down to it from where it is. */
static void set_loop(int a, int b)
{
	int16_t start = (int16_t)(-a * 16), end = (int16_t)(start - b);
	bg.end[0] = bg.end[1] = end;
	bg.start[0] = bg.start[1] = start;
}

/* $5938: the next loop, a * -b - b to a * -b - 1, taken when the drawing
   reaches the end of the current one. */
static void queue_loop(int a, int b)
{
	int16_t start = (int16_t)(-a * b), end = (int16_t)(start - b);
	for (int l = 0; l < 2; l++) {
		bg.next_end[l] = end;
		bg.next_start[l] = start;
		bg.pending[l] = 1;
	}
}

/* $5574: every ring row from the loop's first row. */
static void draw_ring(void)
{
	for (int l = 0; l < 2; l++)
		for (int r = 0; r < RING_ROWS; r++)
			for (int c = 0; c < COLUMNS; c++)
				bg.ring[l][r][c] = metatile(l, (bg.fill[l] >> 5) + r, c);
}

/* $479A: dst = from + (to - from) * w >> shift for a quarter of the
   palette (t & 3), w = (t + 4) & ~3; the intensity bit is dropped. */
static void fade(int from, int to, int shift, int t)
{
	int w = (t + 4) & ~3, q = t & 3;
	for (int i = q * 64; i < q * 64 + 64; i++) {
		uint16_t a = palette_entry(from, i), b = palette_entry(to, i);
		int ag = a >> 11, ar = (a >> 6) & 31, ab = (a >> 1) & 31;
		int bg_ = b >> 11, br = (b >> 6) & 31, bb = (b >> 1) & 31;
		int g = ag + ((w * (bg_ - ag)) >> shift);
		int r = ar + ((w * (br - ar)) >> shift);
		int bl = ab + ((w * (bb - ab)) >> shift);
		bg.palette[i] = (uint16_t)(bl << 1 | r << 6 | g << 11);
	}
}

/* $4864: dst = src * w >> shift (from black), same quarters. */
static void fade_in(int src, int shift, int t)
{
	int w = (t + 4) & ~3, q = t & 3;
	for (int i = q * 64; i < q * 64 + 64; i++) {
		uint16_t a = palette_entry(src, i);
		int g = (w * (a >> 11)) >> shift, r = (w * ((a >> 6) & 31)) >> shift, b = (w * ((a >> 1) & 31)) >> shift;
		bg.palette[i] = (uint16_t)(b << 1 | r << 6 | g << 11);
	}
}

void crs_bg_game_start(int stage)
{
	crs_bg_reset();
	bg.active = 1;
	/* $5906: the loop of the stage, ((stage & 7) - 1) * 8, from its first row. */
	set_loop(((stage & 7) - 1) * 8, 16);
	bg.fill[0] = (int16_t)(bg.end[0] << 5);
	bg.fill[1] = (int16_t)(bg.end[1] << 5);
	if (crs_bg_available()) draw_ring();
}

/* _WIPE (IOCS $B5, the menus after a game): GVRAM cleared, the ring with
   it (nothing drawn until the next game starts). */
void crs_bg_wipe(void)
{
	memset(bg.ring, 0, sizeof bg.ring);
}

/* $56AE, once per game frame. */
void crs_bg_frame(void)
{
	if (!bg.active || !crs_bg_available()) return;

	for (int l = 0; l < 2; l++) {
		if (bg.vel[l] < bg.target[l]) bg.vel[l]++;
		if (bg.vel[l] > bg.target[l]) bg.vel[l]--;
		int32_t old = bg.pos[l], now = old + bg.vel[l];
		bg.pos[l] = now;
		int16_t delta = (int16_t)((now >> 8) - (old >> 8));
		int16_t line = bg.line[l], fill = bg.fill[l];

		/* Down (backwards: some bosses' page 1): SZ2.X draws nothing and
		   keeps its line, so when the layer goes up again it draws the
		   next rows where the last ones are, which may then be on screen
		   (the rows appear there). Here the drawing waits until the
		   screen is back where it stopped: the lines scrolled down are
		   counted (modulo GVRAM's 512, which the screen repeats) and
		   scrolled up again first. */
		if (delta > 0) {
			bg.back[l] = (int16_t)((bg.back[l] + delta) & 511);
			delta = 0;
		} else if (delta < 0 && bg.back[l]) {
			int16_t take = -delta < bg.back[l] ? (int16_t)-delta : bg.back[l];
			bg.back[l] = (int16_t)(bg.back[l] - take);
			delta = (int16_t)(delta + take);
		}
		int16_t n = (int16_t)~delta;	/* lines to go through, minus one */

		/* Up (the normal way): the lines the scroll passes, from line down
		   to line + delta + 1; every fourth one draws one metatile of the
		   ring row 64 lines above it (off the 256 visible ones) from the
		   current map row, and the last of a row (column 0) moves to the
		   map row above, back to the loop's last when it was its first. */
		for (int16_t d = n; d >= 0; d--) {
			int16_t y = (int16_t)(line - n + d);
			if (y & 3) continue;
			int column = (y >> 2) & 7;
			int16_t row = (int16_t)(fill >> 5);
			uint8_t id = metatile(l, row, column);
			bg.ring[l][(y >> 5) & 15][column] = id;
			if (column) continue;
			if (row == bg.end[l]) {
				if (bg.pending[l]) {
					bg.pending[l] = 0;
					row = bg.next_start[l];
					bg.end[l] = bg.next_end[l];
					bg.start[l] = bg.next_start[l];
				} else {
					row = bg.start[l];
				}
			}
			row--;
			fill = (int16_t)(row << 5);
		}
		line = (int16_t)(line + delta);
		bg.line[l] = line;
		bg.fill[l] = fill;
	}

	/* The scroll the next frame shows (taken before $56AE, after the
	   previous frame's: the same). */
	bg.shown[0] = bg.pos[0];
	bg.shown[1] = bg.pos[1];
}

/* $5A3A's colour cycling, every VBL: graphics colours $91, $A1, $D1, $E1. */
void crs_bg_vbl(void)
{
	int32_t v = bg.cycle + bg.cycle_step;
	if ((uint32_t)v > 0x7FF) bg.cycle_step = -bg.cycle_step;
	else bg.cycle = v;
}

/* --- the stage scripts' actions --- */

enum { EV_LOOP, EV_SPEED, EV_FADE, EV_FADE_IN, EV_BLACK, EV_LAST_LOOP };

typedef struct {
	uint8_t stage;
	uint8_t op;
	uint16_t phase;
	int16_t time;		/* -1: every time below arg b (fades: t = time) */
	int16_t a, b;
} event_t;

#define ANY_BELOW -1

/* From the ver1.10 stage scripts ($6DE4 to $D7BE). Actions done when a
   phase changes are listed at the new phase's time 0. */
static const event_t events[] = {
	/* stage 1 ($6DE4) */
	{ 1, EV_SPEED, 0x100, 0, -0x400, -0x100 },
	{ 1, EV_FADE, 0x100, ANY_BELOW, PAL_ST0, PAL_FB_NW },
	{ 1, EV_LOOP, 0x110, 0x200, 2, 16 },
	{ 1, EV_LOOP, 0x130, 0x100, 4, 16 },
	{ 1, EV_LOOP, 0x200, 0, 6, 16 },
	{ 1, EV_SPEED, 0x200, 0, -0x200, 0x80 },
	{ 1, EV_SPEED, 0x210, 0, -0x400, -0x100 },
	{ 1, EV_LOOP, 0x210, 0x200, 8, 16 },
	/* stage 2 ($77B6) */
	{ 2, EV_SPEED, 0x100, 0, -0x400, -0x100 },
	{ 2, EV_FADE, 0x100, ANY_BELOW, PAL_FB_NW, PAL_FY_NP },
	{ 2, EV_LOOP, 0x128, 0, 10, 16 },
	{ 2, EV_LOOP, 0x150, 0xC0, 12, 16 },
	{ 2, EV_LOOP, 0x200, 0, 14, 16 },
	{ 2, EV_LOOP, 0x210, 0x200, 16, 16 },
	/* stage 3 ($8492) */
	{ 3, EV_SPEED, 0x100, 0, -0x400, -0x100 },
	{ 3, EV_FADE, 0x100, ANY_BELOW, PAL_FY_NP, PAL_FY_NR },
	{ 3, EV_LOOP, 0x120, 0x40, 18, 16 },
	{ 3, EV_LOOP, 0x170, 0, 20, 16 },
	{ 3, EV_LOOP, 0x200, 0x100, 22, 16 },
	{ 3, EV_LOOP, 0x210, 0x200, 24, 16 },
	/* stage 4 ($9138) */
	{ 4, EV_SPEED, 0x100, 0, -0x400, -0x100 },
	{ 4, EV_FADE, 0x100, ANY_BELOW, PAL_FY_NR, PAL_FB_NP },
	{ 4, EV_LOOP, 0x120, 0, 26, 16 },
	{ 4, EV_LOOP, 0x140, 0x80, 28, 16 },
	{ 4, EV_LOOP, 0x200, 0, 30, 16 },
	{ 4, EV_SPEED, 0x200, 0x180, -0x400, 0x80 },
	{ 4, EV_LOOP, 0x300, 0x200, 32, 16 },
	{ 4, EV_SPEED, 0x300, 0x200, -0x400, -0x100 },
	/* stage 5 ($A4E8) */
	{ 5, EV_SPEED, 0x100, 0, -0x400, -0x200 },
	{ 5, EV_FADE, 0x100, ANY_BELOW, PAL_FB_NP, PAL_FR_NB },
	{ 5, EV_LOOP, 0x120, 0, 34, 16 },
	{ 5, EV_LOOP, 0x140, 0x100, 36, 16 },
	{ 5, EV_LOOP, 0x200, 0x100, 38, 16 },
	{ 5, EV_SPEED, 0x200, 0x100, -0x800, -0x400 },
	{ 5, EV_SPEED, 0x210, 0, -0x400, -0x200 },
	{ 5, EV_LOOP, 0x210, 0x200, 40, 16 },
	/* stage 6 ($B0B8) */
	{ 6, EV_SPEED, 0x100, 0, -0x400, 0x80 },
	{ 6, EV_FADE, 0x100, ANY_BELOW, PAL_FR_NB, PAL_FB_NY },
	{ 6, EV_LOOP, 0x170, 0x1C0, 42, 16 },
	{ 6, EV_LOOP, 0x1B0, 0xC0, 44, 16 },
	{ 6, EV_LOOP, 0x200, 0, 46, 16 },
	{ 6, EV_SPEED, 0x200, 0, -0x200, -0x40 },
	{ 6, EV_BLACK, 0x210, 0xA0, 0, 0 },
	{ 6, EV_LOOP, 0x210, 0xA0, 48, 16 },
	{ 6, EV_SPEED, 0x210, 0xC0, -0x400, 0x80 },
	/* stage 7 ($C4B0) */
	{ 7, EV_LOOP, 0x100, 0, 48, 16 },
	{ 7, EV_SPEED, 0x100, 0, -0x400, 0x80 },
	{ 7, EV_FADE_IN, 0x100, ANY_BELOW, PAL_ST0, 0 },
	{ 7, EV_LOOP, 0x150, 0, 50, 16 },
	{ 7, EV_LOOP, 0x170, 0, 52, 16 },
	{ 7, EV_LOOP, 0x200, 0, 54, 16 },
	/* first round: on to stage 1's loop, through the end of the map */
	{ 7, EV_LAST_LOOP, 0x210, 0, 55, 16 },
};

void crs_bg_script(int stage, int phase, int time)
{
	if (!bg.active) return;
	int s = stage & 15;
	for (unsigned i = 0; i < sizeof events / sizeof events[0]; i++) {
		const event_t *e = &events[i];
		if (e->stage != s || e->phase != phase) continue;
		if (e->time == ANY_BELOW) {
			if (time < 0 || time >= 0x80) continue;
		} else if (e->time != time) {
			continue;
		}
		switch (e->op) {
		case EV_LOOP:
			set_loop(e->a, e->b);
			break;
		case EV_LAST_LOOP:
			if (stage & 0x10) break;	/* second round: the ending */
			set_loop(e->a, e->b);
			queue_loop(0, 16);
			bg.target[0] = -0x400;
			bg.target[1] = -0x100;
			break;
		case EV_SPEED:
			bg.target[0] = e->a;
			bg.target[1] = e->b;
			break;
		case EV_FADE:
			if (crs_bg_available()) fade(e->a, e->b, 7, time);
			break;
		case EV_FADE_IN:
			if (crs_bg_available()) fade_in(e->a, 7, time);
			break;
		case EV_BLACK:
			memset(bg.palette, 0, sizeof bg.palette);
			break;
		}
	}
}

/* --- display --- */

int crs_bg_active(void)
{
	return crs_bg_enabled && bg.active && crs_bg_available();
}

uint16_t crs_bg_colour(int i)
{
	if (i == 0x91 || i == 0xA1 || i == 0xD1 || i == 0xE1) return (uint16_t)bg.cycle;
	return bg.palette[i & 255];
}

/* GVRAM line y (0-511) of page `layer`, x from 0, count pixels (graphics
   palette indices, 0 transparent): ring row (y / 32 + 2) & 15, its
   metatiles' 4 x 4 cells (tile, 16-colour bank, flips) of 8 x 8 4-bit
   tiles, as $301F6 draws them. */
void crs_bg_line(int layer, int y, int x, int count, uint8_t *out)
{
	const uint8_t *ring = bg.ring[layer][((y >> 5) + SLOT_OFFSET) & 15];
	int cell_row = (y & 31) >> 3, line = y & 7;
	for (int i = 0; i < count;) {
		int gx = (x + i) & 511;
		if (gx >= COLUMNS * 32) {	/* the right half: not drawn here */
			out[i++] = 0;
			continue;
		}
		uint16_t w = be16(data.chp + (ring[gx >> 5] * 16 + cell_row * 4 + ((gx & 31) >> 3)) * 2);
		int tile_line = (w & 0x8000) ? 7 - line : line;
		const uint8_t *t = data.pcg + (w & 0xFF) * 32 + tile_line * 4;
		uint8_t bank = (w >> 4) & 0xF0;
		int flip_x = w & 0x4000;
		for (int px = gx & 7; px < 8 && i < count; px++, i++) {
			int sx = flip_x ? 7 - px : px;
			int c = (t[sx >> 1] >> ((sx & 1) ? 0 : 4)) & 15;
			out[i] = c ? bank | c : 0;
		}
	}
}

int crs_bg_scroll(int layer)
{
	return (bg.shown[layer] >> 8) & 511;
}

/* --- save states --- */

/* The fields in save state order, little-endian. */
static int32_t *const fields32[] = {
	&bg.pos[0], &bg.pos[1], &bg.vel[0], &bg.vel[1], &bg.target[0], &bg.target[1],
	&bg.shown[0], &bg.shown[1], &bg.cycle, &bg.cycle_step,
};
static int16_t *const fields16[] = {
	&bg.line[0], &bg.line[1], &bg.fill[0], &bg.fill[1], &bg.end[0], &bg.end[1],
	&bg.start[0], &bg.start[1], &bg.pending[0], &bg.pending[1],
	&bg.next_start[0], &bg.next_start[1], &bg.next_end[0], &bg.next_end[1], &bg.back[0], &bg.back[1],
	&bg.active,
};
#define FIELDS32 (sizeof fields32 / sizeof fields32[0])
#define FIELDS16 (sizeof fields16 / sizeof fields16[0])

uint32_t crs_bg_state_save(uint8_t *out)
{
	if (out) {
		for (unsigned i = 0; i < FIELDS32; i++)
			for (int k = 0; k < 4; k++) *out++ = (uint32_t)*fields32[i] >> (8 * k);
		for (unsigned i = 0; i < FIELDS16; i++)
			for (int k = 0; k < 2; k++) *out++ = (uint16_t)*fields16[i] >> (8 * k);
		memcpy(out, bg.ring, sizeof bg.ring);
		out += sizeof bg.ring;
		for (int i = 0; i < 256; i++)
			for (int k = 0; k < 2; k++) *out++ = bg.palette[i] >> (8 * k);
	}
	return FIELDS32 * 4 + FIELDS16 * 2 + sizeof bg.ring + sizeof bg.palette;
}

const uint8_t *crs_bg_state_load(const uint8_t *in)
{
	for (unsigned i = 0; i < FIELDS32; i++, in += 4)
		*fields32[i] = (int32_t)(in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24);
	for (unsigned i = 0; i < FIELDS16; i++, in += 2) *fields16[i] = (int16_t)(in[0] | in[1] << 8);
	memcpy(bg.ring, in, sizeof bg.ring);
	in += sizeof bg.ring;
	for (int i = 0; i < 256; i++, in += 2) bg.palette[i] = in[0] | in[1] << 8;
	return in;
}

/* --- the files (PC, from the image) --- */

#ifndef CRS_GNW
#include <stdio.h>
#include <stdlib.h>

int crs_bg_load(const char *dir)
{
	static uint8_t blob[CRS_BG_DATA_SIZE];
	static const struct { const char *name; uint32_t skip, offset, size; } files[] = {
		{ "bg_pcg.sp", 0, 0x0000, 0x2000 },
		{ "map.chp", 8, 0x2000, 0x2000 },
		{ "map.bg0", 16, 0x4000, 0x2000 },
		{ "map.bg1", 16, 0x6000, 0x2000 },
		{ "bg_fb_nw.pal", 0, 0x8000 + 0 * 512, 512 },
		{ "bg_fy_np.pal", 0, 0x8000 + 1 * 512, 512 },
		{ "bg_fy_nr.pal", 0, 0x8000 + 2 * 512, 512 },
		{ "bg_fb_np.pal", 0, 0x8000 + 3 * 512, 512 },
		{ "bg_fr_nb.pal", 0, 0x8000 + 4 * 512, 512 },
		{ "bg_fb_ny.pal", 0, 0x8000 + 5 * 512, 512 },
		{ "bg_st0.pal", 0, 0x8000 + 6 * 512, 512 },
		{ "bg_dark.pal", 0, 0x8000 + 7 * 512, 512 },
		{ "bg_lite.pal", 0, 0x8000 + 8 * 512, 512 },
	};
	for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, files[i].name);
		FILE *f = fopen(path, "rb");
		if (!f) {
			CRS_LOG("%s: cannot open (ver1.10 backgrounds)\n", path);
			crs_bg_set_data(NULL, 0);
			return -1;
		}
		int ok = !fseek(f, files[i].skip, SEEK_SET) && fread(blob + files[i].offset, 1, files[i].size, f) == files[i].size;
		fclose(f);
		if (!ok) {
			CRS_LOG("%s: too short\n", path);
			crs_bg_set_data(NULL, 0);
			return -1;
		}
	}
	return crs_bg_set_data(blob, sizeof blob);
}
#endif
