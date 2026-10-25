/*
 * The X68000 screen from the game's memory, as the Amiga port's graphics.s
 * draws it, one line at a time straight into the front end's frame buffer
 * (no screen-sized buffer: RAM is short on the Game & Watch).
 *
 * Layers, back to front: GVRAM page 1 and page 0 (256 colours each, their
 * own scroll registers, colour 0 transparent; where both are, the backdrop:
 * sprite palette entry 0), then the text layer (4 bit planes, palette block
 * 0) and the sprites (16 x 16, 4 bits per pixel, 16 palettes), in the order
 * video controller register R1 sets. Pixels are X68000 palette indices
 * until the end of each line: 0-255 graphics, 256-511 sprites and text.
 */

#include <string.h>
#include "crs.h"
#include "crs_symbols.h"

#define GUARD 16	/* sprite coordinates start 16 pixels off screen */
#define LINE (GUARD + CRS_SCREEN_WIDTH + GUARD)
#define PCG_PATTERNS 0x75E

#define GVRAM 0xC00000u
#define GVRAM_PAGE_SIZE 0x80000u
#define TEXT_VRAM 0xE00000u
#define TEXT_PLANE_SIZE 0x20000u
#define CRTC 0xE80000u
#define PALETTES 0xE82000u	/* graphics (256), then sprites and text (256) */
#define VC_R1 0xE82500u

static uint16_t colours[512];
static uint16_t order[CRS_MAXIMUM_SPRITES];
static int order_count;
static uint32_t pattern_data;

/* X68000 colour (G5 R5 B5 I1) -> RGB565 (the intensity bit is green's low bit). */
static uint16_t rgb565(uint32_t c)
{
	uint32_t g = (c >> 11) & 31, r = (c >> 6) & 31, b = (c >> 1) & 31;
	return r << 11 | (g << 1 | (c & 1)) << 5 | b;
}

/* From a pack's layers once the game has built GVRAM (until then it is
   empty: the backdrop), or from GVRAM through the 68000 memory (the low
   byte of each word). */
static void graphics_line(int y, uint16_t *out)
{
	if (crs_bg_active()) {
		/* The ver1.10 backgrounds (crs_bg.c): page 0 in front of page 1,
		   no horizontal scroll. */
		uint8_t line0[CRS_SCREEN_WIDTH], line1[CRS_SCREEN_WIDTH];
		crs_bg_line(0, (crs_bg_scroll(0) + y) & 511, 0, CRS_SCREEN_WIDTH, line0);
		crs_bg_line(1, (crs_bg_scroll(1) + y) & 511, 0, CRS_SCREEN_WIDTH, line1);
		for (int x = 0; x < CRS_SCREEN_WIDTH; x++) {
			uint8_t c = line0[x] ? line0[x] : line1[x];
			out[x] = c ? c : 256;
		}
		return;
	}

	uint32_t x0 = crs_rd16(CRTC + 0x18) & 511, y0 = crs_rd16(CRTC + 0x1A) & 511;
	uint32_t x1 = crs_rd16(CRTC + 0x20) & 511, y1 = crs_rd16(CRTC + 0x22) & 511;

	if (crs_layers[0] && crs_layers[1]) {
		if (!crs_graphics_builds) {
			for (int x = 0; x < CRS_SCREEN_WIDTH; x++) out[x] = 256;
			return;
		}
		const uint8_t *line0 = crs_layers[0] + ((y0 + y) & 511) * 512;
		const uint8_t *line1 = crs_layers[1] + ((y1 + y) & 511) * 512;
		for (int x = 0; x < CRS_SCREEN_WIDTH; x++) {
			uint8_t c = line0[(x0 + x) & 511];
			if (!c) c = line1[(x1 + x) & 511];
			out[x] = c ? c : 256;
		}
		return;
	}

	uint32_t line0 = GVRAM + ((y0 + y) & 511) * 1024 + 1;
	uint32_t line1 = GVRAM + GVRAM_PAGE_SIZE + ((y1 + y) & 511) * 1024 + 1;
	for (int x = 0; x < CRS_SCREEN_WIDTH; x++) {
		uint8_t c = crs_rd8(line0 + ((x0 + x) & 511) * 2);
		if (!c) c = crs_rd8(line1 + ((x1 + x) & 511) * 2);
		out[x] = c ? c : 256;
	}
}

/* Text: 4 planes of 1024 x 1024 pixels, 128 bytes a line (within one
   256-byte page); colour 0 is transparent. */
static void text_line(int y, uint16_t *out)
{
	uint32_t sx = crs_rd16(CRTC + 0x14) & 1023, sy = crs_rd16(CRTC + 0x16) & 1023;
	uint32_t line = TEXT_VRAM + ((sy + y) & 1023) * 128;
	const uint8_t *plane[4];
	int empty = 1;
	for (int p = 0; p < 4; p++) {
		const uint8_t *page = crs_page_read(line + p * TEXT_PLANE_SIZE);
		plane[p] = page + (line & 255);
		if (page != crs_zero_page) empty = 0;	/* nothing written there */
	}
	if (empty) return;

	for (int x = 0; x < CRS_SCREEN_WIDTH; x++) {
		uint32_t tx = (sx + x) & 1023, byte = tx >> 3, bit = 7 - (tx & 7);
		int c = (plane[0][byte] >> bit & 1)
		      | (plane[1][byte] >> bit & 1) << 1
		      | (plane[2][byte] >> bit & 1) << 2
		      | (plane[3][byte] >> bit & 1) << 3;
		if (c) out[x] = 256 + c;
	}
}

/* Sprites in priority order, as the ports: 32 buckets by attribute bits
   4-0, drawn from 0 to 31 (later is in front), the entries of a bucket in
   reverse list order. */
static void order_sprites(void)
{
	order_count = 0;
	pattern_data = crs_rd32(CRS_SYM_SPRITE_DATA_ADDRESS);
	if (!pattern_data) return;
	for (int bucket = 0; bucket < 32; bucket++)
		for (int i = crs_sprite_count - 1; i >= 0; i--)
			if ((crs_sprite_list[i * 8 + 7] & 0x1F) == bucket) order[order_count++] = i;
}

/* line: the screen line with the guard band (sprite x is its index). */
static void sprites_line(int y, uint16_t *line)
{
	uint32_t ly = y + GUARD;	/* sprite coordinates */

	for (int k = 0; k < order_count; k++) {
		const uint8_t *s = crs_sprite_list + order[k] * 8;
		uint32_t x = s[0] << 8 | s[1], sy = s[2] << 8 | s[3];
		if (ly < sy || ly >= sy + 16 || x > LINE - 16 || sy > GUARD + CRS_SCREEN_HEIGHT) continue;
		uint32_t pattern = s[4] << 8 | s[5], attributes = s[6] << 8 | s[7];
		if (pattern >= PCG_PATTERNS) continue;

		uint32_t row = ly - sy;
		if (attributes & 0x8000) row = 15 - row;
		uint32_t source = (pattern_data + pattern * 128) & 0xFFFFFF;
		uint32_t left = crs_rd32(source + row * 4), right = crs_rd32(source + 64 + row * 4);
		uint16_t palette = 256 + ((attributes >> 8) & 15) * 16;
		uint16_t *out = line + x;
		int flip_x = attributes & 0x4000;

		for (int column = 0; column < 16; column++) {
			uint32_t word = column < 8 ? left : right;
			int c = word >> (28 - 4 * (column & 7)) & 15;
			if (c) out[flip_x ? 15 - column : column] = palette + c;
		}
	}
}

void crs_render(uint16_t *out, int stride, int first, int count)
{
	int bg = crs_bg_active();
	for (int i = 0; i < 512; i++)
		colours[i] = rgb565(bg && i < 256 ? crs_bg_colour(i) : crs_rd16(PALETTES + i * 2));
	order_sprites();

	/* R1 bits 13-12 = sprite priority, 11-10 = text priority (0 = front). */
	uint32_t r1 = crs_rd16(VC_R1);
	int text_in_front = ((r1 >> 12) & 3) > ((r1 >> 10) & 3);

	uint16_t line[LINE];
	for (int y = first; y < first + count && y < CRS_SCREEN_HEIGHT; y++) {
		graphics_line(y, line + GUARD);
		if (text_in_front) {
			sprites_line(y, line);
			text_line(y, line + GUARD);
		} else {
			text_line(y, line + GUARD);
			sprites_line(y, line);
		}
		uint16_t *o = out + (y - first) * stride;
		for (int x = 0; x < CRS_SCREEN_WIDTH; x++) o[x] = colours[line[GUARD + x]];
	}
}
