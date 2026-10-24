/*
 * IMA ADPCM (4 bits a sample, 16-bit signal), for the music in the pack:
 * tools/make_crs.c encodes, crs_audio.c decodes, with the same code so the
 * encoder's decoder state is the player's.
 */

#ifndef CRS_IMA_H
#define CRS_IMA_H

#include <stdint.h>

static const int16_t crs_ima_steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
	97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
	4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
	18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static const int8_t crs_ima_index_steps[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct {
	int32_t predictor;	/* the last sample */
	int32_t index;		/* into crs_ima_steps */
} crs_ima_t;

/* Applies a nibble: the next sample. */
static inline int32_t crs_ima_decode(crs_ima_t *s, int nibble)
{
	int32_t step = crs_ima_steps[s->index];
	int32_t difference = step >> 3;
	if (nibble & 4) difference += step;
	if (nibble & 2) difference += step >> 1;
	if (nibble & 1) difference += step >> 2;
	s->predictor += nibble & 8 ? -difference : difference;
	if (s->predictor > 32767) s->predictor = 32767;
	if (s->predictor < -32768) s->predictor = -32768;
	s->index += crs_ima_index_steps[nibble];
	if (s->index < 0) s->index = 0;
	if (s->index > 88) s->index = 88;
	return s->predictor;
}

/* The nibble for a sample (the state follows as the decoder's would). */
static inline int crs_ima_encode(crs_ima_t *s, int32_t sample)
{
	int32_t step = crs_ima_steps[s->index];
	int32_t difference = sample - s->predictor;
	int nibble = 0;
	if (difference < 0) {
		nibble = 8;
		difference = -difference;
	}
	if (difference >= step) { nibble |= 4; difference -= step; }
	if (difference >= step >> 1) { nibble |= 2; difference -= step >> 1; }
	if (difference >= step >> 2) nibble |= 1;
	crs_ima_decode(s, nibble);
	return nibble;
}

#endif
