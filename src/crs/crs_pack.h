/*
 * The .crs pack: the game as the Game & Watch core loads it from the SD card
 * (/homebrews/ChoRenSha.crs, beside ChoRenSha.bin), made by tools/make_crs.c
 * from game/data. The firmware maps it from external flash, so everything
 * is read in place: little-endian fields, sections aligned to 4 KB.
 *
 * It holds the game as it is once it has loaded its files (the end of its
 * second frame), so the core never loads them, and the backgrounds the game
 * builds in GVRAM when a game starts, so GVRAM needs no RAM:
 *
 *   header       "CRSP", version, section count, then per section its id
 *                (4 characters), offset and size
 *   "PAGE"       u32 count, then the 68000 page numbers (address / 256,
 *                ascending) of every page that is not all zero
 *   "PDAT"       those pages, 256 bytes each, in the same order
 *   "CPU "       the registers (crs_cpu_regs order), u32 each
 *   "PORT"       the port layer's state (crs_state_save)
 *   "GFX0/GFX1"  GVRAM pages 0 and 1: 512 x 512 graphics palette indices
 *   "BG11"       the ver1.10 backgrounds (crs_bg.c): CRS_BG_DATA_SIZE bytes,
 *                big-endian as on the X68000 (layout in crs.h)
 *   "ID  "       u32: the game image's checksum (CRS_IMAGE_ID); the core
 *                knows the image's symbols, so it only takes packs made
 *                from the same image
 *   "MUSI"       the music (optional): the songs as mono IMA ADPCM
 *                (crs_ima.h), read in place from flash like the rest:
 *                crs_pack_music_t, then song_count crs_pack_song_t, then
 *                the data. Each song is an intro and a loop (no loop for
 *                a song that ends), each its own stream of nibbles (low
 *                nibble first) with a checkpoint (crs_pack_checkpoint_t,
 *                the decoder's state) every checkpoint_interval samples,
 *                from the stream's start, so the player can start
 *                anywhere (the loop, a save state).
 */

#ifndef CRS_PACK_H
#define CRS_PACK_H

#include <stdint.h>

#define CRS_PACK_MAGIC "CRSP"
#define CRS_PACK_VERSION 1
#define CRS_PACK_ALIGN 4096
#define CRS_PACK_PAGE 256
#define CRS_PACK_LAYER_SIZE (512 * 512)

typedef struct {
	char magic[4];
	uint32_t version;
	uint32_t section_count;
} crs_pack_header_t;

/* "MUSI": offsets are from the section's start. */
typedef struct {
	uint32_t song_count;
	uint32_t checkpoint_interval;	/* samples */
} crs_pack_music_t;

typedef struct {
	char name[16];			/* the song's file name in BGM_DAT, without .MDC */
	uint32_t rate;			/* Hz */
	uint32_t samples[2];		/* intro, loop (0: none) */
	uint32_t level;			/* 1-64: the song's original level (the streams are normalised) */
	uint32_t fade_unit;		/* MCDRV _FADEOUT length per unit of speed, 1/10000 s */
	uint32_t data[2];		/* intro, loop: their nibbles */
	uint32_t checkpoints[2];	/* intro, loop: their checkpoints */
	uint32_t reserved[3];
} crs_pack_song_t;

typedef struct {
	int16_t predictor;
	uint8_t index;
	uint8_t reserved;
} crs_pack_checkpoint_t;

typedef struct {
	char id[4];
	uint32_t offset;
	uint32_t size;
} crs_pack_section_t;

/* The section with this id, or NULL; *size gets its size. */
static inline const uint8_t *crs_pack_section(const uint8_t *pack, uint32_t pack_size, const char *id, uint32_t *size)
{
	const crs_pack_header_t *h = (const crs_pack_header_t *)pack;
	if (pack_size < sizeof *h || h->magic[0] != 'C' || h->magic[1] != 'R' || h->magic[2] != 'S' || h->magic[3] != 'P'
	    || h->version != CRS_PACK_VERSION)
		return 0;
	const crs_pack_section_t *s = (const crs_pack_section_t *)(h + 1);
	for (uint32_t i = 0; i < h->section_count; i++) {
		if (s[i].id[0] == id[0] && s[i].id[1] == id[1] && s[i].id[2] == id[2] && s[i].id[3] == id[3]
		    && s[i].offset + s[i].size <= pack_size) {
			if (size) *size = s[i].size;
			return pack + s[i].offset;
		}
	}
	return 0;
}

#endif
