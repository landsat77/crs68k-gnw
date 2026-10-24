/*
 * Sound, as the Amiga port's audio.s plays it, mixed to mono, with no large
 * buffers (the Game & Watch's RAM):
 *
 * - Effects: the X68000's single ADPCM channel (_ADPCMOUT), decoded while
 *   it plays from the game's memory, following MAME's okim6258 as the
 *   X68000 wires it (10-bit output, signal -2 and step 0 at each start, low
 *   nibble first). A new effect replaces the one playing.
 * - Music: MCDRV (trap #4) is answered with the songs pre-rendered by
 *   tools/music/build_music.sh (intro, then the loop for ever): from the
 *   pack's "MUSI" section (mono IMA ADPCM, decoded in place from flash),
 *   or else from NAME.crm, read a block at a time. Linearly interpolated to
 *   the output rate, with the song's level and MCDRV's fade (64 steps of
 *   0.75 dB over speed * fade_unit / 10000 seconds, then silence).
 *
 * crs_audio_lock, when the front end sets it, guards the state shared with
 * its audio thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef CRS_GNW
#include <dirent.h>
#endif
#include "crs.h"
#include "crs_pack.h"
#include "crs_ima.h"

void (*crs_audio_lock)(int lock);

#define LOCK() do { if (crs_audio_lock) crs_audio_lock(1); } while (0)
#define UNLOCK() do { if (crs_audio_lock) crs_audio_lock(0); } while (0)

#define EFFECT_GAIN 24		/* 10-bit signal -> 16 bits, about SFXVOL 32 of 64 */
#define MUSIC_GAIN 512		/* 16-bit music at level 64 -> half scale, 1/65536 */
#define FADE_STEPS 64

/* --- effects --- */

/* floor(16 * 1.1^step), as MAME computes it. */
static const int16_t steps[49] = {
	16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157,
	173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
	1166, 1282, 1411, 1552,
};
static const int8_t step_shifts[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static struct {
	uint32_t address, nibbles, next;	/* next: nibbles decoded so far */
	int value, step;
	uint32_t rate;
	uint32_t phase;			/* 16.16 source samples into the current one */
} effect;

static void decode_nibble(void)
{
	int nibble = crs_rd8(effect.address + effect.next / 2) >> (effect.next & 1 ? 4 : 0) & 15;
	int s = steps[effect.step];
	int d = s / 8;
	if (nibble & 1) d += s / 4;
	if (nibble & 2) d += s / 2;
	if (nibble & 4) d += s;
	effect.value += nibble & 8 ? -d : d;
	if (effect.value > 511) effect.value = 511;
	if (effect.value < -512) effect.value = -512;
	effect.step += step_shifts[nibble & 7];
	if (effect.step < 0) effect.step = 0;
	if (effect.step > 48) effect.step = 48;
	effect.next++;
}

void crs_audio_effect(uint32_t address, uint32_t length, int rate_index, int channels)
{
	static const uint32_t rates[] = { 3906, 5208, 7813, 10417, 15625 };
	(void)channels;	/* mono */

	LOCK();
	effect.address = address;
	effect.nibbles = length * 2;
	effect.next = 0;
	effect.value = -2;
	effect.step = 0;
	effect.rate = rates[rate_index];
	effect.phase = 0;
	if (effect.nibbles) decode_nibble();
	UNLOCK();
}

void crs_audio_effect_stop(void)
{
	LOCK();
	effect.nibbles = 0;
	UNLOCK();
}

/* --- music --- */

#define MAXIMUM_SONGS 32
#define MAXIMUM_BLOCK 4096

static int music_enabled;
static char music_dir[1024];
static struct { uint32_t data; char name[64]; } songs[MAXIMUM_SONGS];
static int song_count, transferred = -1;

static const crs_pack_music_t *pack_music;	/* the pack's songs, or NULL */
static const crs_pack_song_t *pack_songs;

static struct {
	int song;			/* index in songs, -1: none (save states) */
	const crs_pack_song_t *packed;	/* from the pack, or from file: */
	FILE *file;
	uint32_t intro, loop, block, rate, level, fade_unit;
	uint32_t position;		/* frame */
	uint32_t phase;			/* the fraction of a frame, 1/2^32 */
	int previous;			/* the frame before position, for the interpolation */
	/* From file: the block in buffer. */
	int cached_part, cached_block;
	uint32_t cached_frames;
	int8_t buffer[2 * MAXIMUM_BLOCK];	/* its left samples, then its right */
	/* From the pack: the decoder, its part and the next sample it decodes. */
	crs_ima_t ima;
	int ima_part;
	uint32_t ima_next;
	int playing;
	uint32_t fade_samples, fade_done;	/* output samples; 0: no fade */
} music = { .song = -1, .cached_part = -1, .ima_part = -1 };

void crs_audio_pack_music(const uint8_t *section, uint32_t size)
{
	pack_music = NULL;
	if (!section || size < sizeof *pack_music) return;
	const crs_pack_music_t *h = (const crs_pack_music_t *)section;
	if (!h->checkpoint_interval || sizeof *h + (uint64_t)h->song_count * sizeof *pack_songs > size) return;
	pack_music = h;
	pack_songs = (const crs_pack_song_t *)(h + 1);
}

/* The pack's song with this name (any case), or NULL. */
static const crs_pack_song_t *pack_song(const char *name)
{
	if (!pack_music) return NULL;
	for (uint32_t s = 0; s < pack_music->song_count; s++) {
		const char *a = pack_songs[s].name;
		int i = 0;
		while (i < (int)sizeof pack_songs[s].name && a[i] && name[i] && (a[i] & ~0x20) == (name[i] & ~0x20)) i++;
		if (i < (int)sizeof pack_songs[s].name && !a[i] && !name[i]) return &pack_songs[s];
	}
	return NULL;
}

static int16_t fade_gain[FADE_STEPS + 1];	/* 1/32768 */

/* On the Game & Watch (CRS_GNW) the core's settings say; a missing stream
   then plays nothing. */
int crs_audio_music_available(void)
{
	if (!music_enabled) return 0;
#ifdef CRS_GNW
	return 1;
#else
	if (pack_music && pack_music->song_count) return 1;
	DIR *d = opendir(music_dir);
	if (!d) return 0;
	closedir(d);
	return 1;
#endif
}

/* After the game loaded a song: its data address and file name
   ("BGM_DAT\SZ2_S1.mdc" -> stream "SZ2_S1"). */
void crs_audio_song_loaded(uint32_t data, const char *name)
{
	if (song_count == MAXIMUM_SONGS) return;
	const char *base = name;
	for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') base = c + 1;
	int n = 0;
	while (base[n] && base[n] != '.' && base[n] > ' ' && n < 63) n++;
	songs[song_count].data = data;
	snprintf(songs[song_count].name, sizeof songs[song_count].name, "%.*s", n, base);
	song_count++;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static void stop_music(void)
{
	if (music.file) fclose(music.file);
	music.file = NULL;
	music.packed = NULL;
	music.playing = 0;
	music.song = -1;
}

/* The pack's song, or else NAME.crm and its header (32 bytes, big-endian). */
static void start_music(const char *name)
{
	music.position = 0;
	music.phase = 0;
	music.previous = 0;
	music.cached_part = -1;
	music.ima_part = -1;
	music.fade_samples = 0;

	const crs_pack_song_t *song = pack_song(name);
	if (song) {
		music.packed = song;
		music.rate = song->rate;
		music.intro = song->samples[0];
		music.loop = song->samples[1];
		music.fade_unit = song->fade_unit;
		music.level = song->level;
		music.playing = 1;
		return;
	}

	char path[1200];
	snprintf(path, sizeof path, "%s/%s.crm", music_dir, name);
	FILE *f = fopen(path, "rb");
	uint8_t header[32];
	if (!f || fread(header, 1, 32, f) != 32 || memcmp(header, "CRSM", 4) || be32(header + 20) > MAXIMUM_BLOCK) {
		if (f) fclose(f);
		CRS_LOG("music: %s not found\n", path);
		return;
	}
	music.file = f;
	music.rate = be32(header + 8);
	music.intro = be32(header + 12);
	music.loop = be32(header + 16);
	music.block = be32(header + 20);
	music.fade_unit = be16(header + 26);
	music.level = be16(header + 28);
	music.playing = 1;
}

/* The mono sample of a frame (16 bits). From the pack: decoded in order,
   from the checkpoint before it when the song jumps (the loop, a save
   state). From file: its block (intro, then loop, each cut in blocks of
   left samples then right samples) read when needed. */
static int music_sample(uint32_t frame)
{
	int part = frame >= music.intro;
	uint32_t in_part = part ? frame - music.intro : frame;
	uint32_t part_frames = part ? music.loop : music.intro;

	if (music.packed) {
		const uint8_t *base = (const uint8_t *)pack_music;
		uint32_t interval = pack_music->checkpoint_interval;
		if (part != music.ima_part || in_part + 1 < music.ima_next || in_part >= music.ima_next + interval) {
			const crs_pack_checkpoint_t *c =
				(const crs_pack_checkpoint_t *)(base + music.packed->checkpoints[part]) + in_part / interval;
			music.ima.predictor = c->predictor;
			music.ima.index = c->index;
			music.ima_part = part;
			music.ima_next = in_part - in_part % interval;
		}
		const uint8_t *nibbles = base + music.packed->data[part];
		while (music.ima_next <= in_part) {
			uint32_t i = music.ima_next++;
			crs_ima_decode(&music.ima, nibbles[i >> 1] >> (i & 1 ? 4 : 0) & 15);
		}
		return music.ima.predictor;
	}

	int block = in_part / music.block;

	if (part != music.cached_part || block != music.cached_block) {
		uint32_t start = 32 + (part ? music.intro * 2 : 0) + block * music.block * 2;
		uint32_t frames = part_frames - block * music.block;
		if (frames > music.block) frames = music.block;
		if (fseek(music.file, start, SEEK_SET) || fread(music.buffer, 1, frames * 2, music.file) != frames * 2) {
			stop_music();
			return 0;
		}
		music.cached_part = part;
		music.cached_block = block;
		music.cached_frames = frames;
	}

	uint32_t i = in_part - block * music.block;
	return (music.buffer[i] + music.buffer[music.cached_frames + i]) * 128;
}

void crs_audio_music_call(uint32_t function, uint32_t d1, uint32_t a0)
{
	switch (function) {
	case 0x01: /* _TRANSMDC: a0 = song data */
		transferred = -1;
		for (int i = 0; i < song_count; i++) if (songs[i].data == a0) transferred = i;
		break;
	case 0x02: /* _PLAYMUSIC */
		LOCK();
		stop_music();
		if (music_enabled && transferred >= 0) {
			start_music(songs[transferred].name);
			if (music.playing) music.song = transferred;
		}
		UNLOCK();
		break;
	case 0x05: /* _STOPMUSIC */
		LOCK();
		stop_music();
		UNLOCK();
		break;
	case 0x14: /* _FADEOUT: d1 = speed; the length is set in samples when mixing */
		LOCK();
		if (music.playing && !music.fade_samples) {
			uint32_t speed = d1 & 0xFFFF ? d1 & 0xFFFF : 1;
			music.fade_samples = speed * music.fade_unit;	/* 1/10000 s until the first mix */
			music.fade_done = (uint32_t)-1;
			if (!music.fade_samples) stop_music();
		}
		UNLOCK();
		break;
	}
}

void crs_audio_vbl(void)
{
}

/* Music on or off while playing (the song starts again with the next one
   the game plays). */
void crs_audio_set_music(int on)
{
	LOCK();
	music_enabled = on;
	if (!on) stop_music();
	UNLOCK();
}

/* The songs the game has loaded and the one transferred, for packs (no
   song plays when the game has just loaded its files). Little-endian. */
uint32_t crs_audio_state_save(uint8_t *out)
{
	uint32_t size = 8 + song_count * (4 + sizeof songs[0].name);
	if (!out) return size;
	uint32_t fields[2] = { song_count, (uint32_t)transferred };
	for (int f = 0; f < 2; f++) for (int i = 0; i < 4; i++) *out++ = fields[f] >> (8 * i);
	for (int s = 0; s < song_count; s++) {
		for (int i = 0; i < 4; i++) *out++ = songs[s].data >> (8 * i);
		memcpy(out, songs[s].name, sizeof songs[s].name);
		out += sizeof songs[s].name;
	}
	return size;
}

const uint8_t *crs_audio_state_load(const uint8_t *in)
{
	uint32_t fields[2];
	for (int f = 0; f < 2; f++, in += 4) fields[f] = in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24;
	song_count = fields[0] > MAXIMUM_SONGS ? MAXIMUM_SONGS : fields[0];
	transferred = (int32_t)fields[1];
	for (int s = 0; s < song_count; s++) {
		songs[s].data = in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24;
		memcpy(songs[s].name, in + 4, sizeof songs[s].name);
		songs[s].name[sizeof songs[s].name - 1] = 0;
		in += 4 + sizeof songs[s].name;
	}
	return in;
}

/* What plays, for save states: the song, its frame and phase, the fade
   (little-endian u32 each; CRS_AUDIO_PLAY_SIZE bytes). The effect playing
   is not kept. */
void crs_audio_play_save(uint8_t *out)
{
	LOCK();
	uint32_t fields[5] = { (uint32_t)(music.playing ? music.song : -1), music.position, music.phase,
	                       music.fade_samples, music.fade_done };
	UNLOCK();
	for (int f = 0; f < 5; f++) for (int i = 0; i < 4; i++) *out++ = fields[f] >> (8 * i);
}

void crs_audio_play_load(const uint8_t *in)
{
	uint32_t fields[5];
	for (int f = 0; f < 5; f++, in += 4) fields[f] = in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24;
	LOCK();
	effect.nibbles = 0;
	stop_music();
	int song = (int32_t)fields[0];
	if (music_enabled && song >= 0 && song < song_count) {
		start_music(songs[song].name);
		if (music.playing) {
			music.song = song;
			music.position = fields[1];
			music.phase = fields[2];
			music.fade_samples = fields[3];
			music.fade_done = fields[4];
			if (music.position < music.intro + music.loop) music.previous = music_sample(music.position);
		}
	}
	UNLOCK();
}

/* --- mixing --- */

void crs_audio_init(const char *directory, int enabled)
{
	snprintf(music_dir, sizeof music_dir, "%s", directory);
	music_enabled = enabled;
	for (int i = 0; i <= FADE_STEPS; i++)
		fade_gain[i] = i == FADE_STEPS ? 0 : (int16_t)(32767 * pow(10.0, -0.75 * i / 20.0));
}

void crs_audio_mix(int16_t *out, int samples, int rate)
{
	uint32_t effect_step = ((uint64_t)effect.rate << 16) / rate;
	uint64_t music_step = ((uint64_t)music.rate << 32) / rate;

	/* A fade's length in output samples, now that the rate is known. */
	if (music.fade_samples && music.fade_done == (uint32_t)-1) {
		music.fade_samples = (uint64_t)music.fade_samples * rate / 10000;
		if (!music.fade_samples) music.fade_samples = 1;
		music.fade_done = 0;
	}

	for (int i = 0; i < samples; i++) {
		int v = 0;

		if (effect.next <= effect.nibbles && effect.nibbles) {
			v += effect.value * EFFECT_GAIN;
			effect.phase += effect_step;
			while (effect.phase >= 0x10000 && effect.nibbles) {
				effect.phase -= 0x10000;
				if (effect.next < effect.nibbles) decode_nibble();
				else effect.nibbles = 0;
			}
		}

		if (music.playing) {
			if (music.position >= music.intro + music.loop) {
				if (music.loop) music.position -= music.loop;
				else stop_music();
			}
			if (music.playing) {
				int gain = music.level * MUSIC_GAIN;
				if (music.fade_samples) {
					uint32_t step = (uint64_t)music.fade_done * FADE_STEPS / music.fade_samples;
					if (step >= FADE_STEPS) stop_music();
					else gain = gain * fade_gain[step] >> 15;
					music.fade_done++;
				}
				if (music.playing) {
					int current = music_sample(music.position);
					int s = music.previous + (int)((int64_t)(current - music.previous) * (music.phase >> 16) >> 16);
					v += (int)((int64_t)s * gain >> 16);
					uint64_t phase = music.phase + music_step;
					if (phase >> 32) {
						music.previous = current;
						music.position += phase >> 32;
					}
					music.phase = (uint32_t)phase;
				}
			}
		}

		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		out[i] = v;
	}
}
