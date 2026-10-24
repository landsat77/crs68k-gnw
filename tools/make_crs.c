/*
 * make_crs: makes the .crs pack the Game & Watch homebrew loads (format in
 * src/crs/crs_pack.h). Runs the game on the port layer from the linked
 * image and game/data until it has loaded its files (the end of its
 * second frame, all its loads happen before) and takes its state there: the
 * non-zero 256-byte pages of memory, the registers, the port layer's state.
 * Then it lets the attract demo run until the game has built GVRAM (at the
 * start of the demo's game) and takes the two background layers.
 *
 * The backgrounds come from files loaded at start-up only, so every game
 * builds the same ones, except with the rotated screen setting
 * (WORD_0008DAFC), which the pack does not cover.
 *
 * Last, the music: every game/music/NAME.crm (22,050 Hz, 8-bit stereo, from
 * tools/music) mixed to mono, resampled to music_rate (default 11,025 Hz)
 * and encoded as IMA ADPCM (4 bits a sample) into the pack's "MUSI"
 * section, so the device reads it from flash, not from the SD card.
 *
 * usage: make_crs image.bin data_dir music_dir work_dir out.crs [music_rate]
 *
 * data_dir: game/data. music_dir: game/music; with it the game finds a
 * music driver and loads its songs, so the homebrew plays them.
 * work_dir: files the game writes meanwhile.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include "crs.h"
#include "crs_pack.h"
#include "crs_ima.h"
#include "crs_symbols.h"

#define SNAPSHOT_FRAME 2
#define MAXIMUM_FRAMES 5000	/* to reach the demo's game start */

typedef struct {
	const char *id;
	uint8_t *data;
	uint32_t size;
} section_t;

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}

static int run_until(int (*done)(void))
{
	while (!done()) {
		if (crs_run_frame() == CRS_EXITED || crs_frame_count > MAXIMUM_FRAMES) return -1;
	}
	return 0;
}

static int loaded(void) { return crs_frame_count >= SNAPSHOT_FRAME; }
static int built(void) { return crs_graphics_builds > 0; }

/* --- music --- */

#define CHECKPOINT_INTERVAL 4096

static uint32_t be(const uint8_t *p, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++) v = v << 8 | p[i];
	return v;
}

/* A part of a .crm stream (blocks of left samples then right samples,
   signed 8-bit) as mono 16-bit: (left + right) * 128. */
static int32_t *read_part(FILE *f, uint32_t frames, uint32_t block)
{
	int32_t *mono = malloc((frames + 1) * sizeof *mono);
	int8_t *buffer = malloc(2 * block);
	for (uint32_t done = 0; done < frames;) {
		uint32_t n = frames - done < block ? frames - done : block;
		if (fread(buffer, 1, 2 * n, f) != 2 * n) { free(mono); mono = NULL; break; }
		for (uint32_t i = 0; i < n; i++) mono[done + i] = (buffer[i] + buffer[n + i]) * 128;
		done += n;
	}
	free(buffer);
	return mono;
}

/* From rate src to dst, each output sample a triangle-weighted average of
   the input around it (two output periods wide): enough of a low-pass for
   the Game & Watch's speaker. */
static int32_t *resample(const int32_t *in, uint32_t n, uint32_t src, uint32_t dst, uint32_t *out_n)
{
	*out_n = (uint32_t)(((uint64_t)n * dst + src / 2) / src);
	int32_t *out = malloc((*out_n + 1) * sizeof *out);
	double ratio = (double)src / dst, width = ratio > 1 ? ratio : 1;
	for (uint32_t k = 0; k < *out_n; k++) {
		double center = (k + 0.5) * ratio - 0.5, sum = 0, weights = 0;
		for (long i = (long)(center - width) + 1; i <= (long)(center + width); i++) {
			if (i < 0 || i >= (long)n) continue;
			double w = 1 - fabs(i - center) / width;
			if (w <= 0) continue;
			sum += in[i] * w;
			weights += w;
		}
		out[k] = weights > 0 ? (int32_t)(sum / weights) : 0;
	}
	return out;
}

typedef struct {
	uint8_t *nibbles;	/* (samples + 1) / 2 bytes */
	crs_pack_checkpoint_t *checkpoints;
	uint32_t checkpoint_count;
	double signal, noise;	/* for the quality report */
} stream_t;

static stream_t encode(const int32_t *samples, uint32_t n)
{
	stream_t s = { calloc(1, (n + 1) / 2 + 1), NULL, (n + CHECKPOINT_INTERVAL - 1) / CHECKPOINT_INTERVAL, 0, 0 };
	s.checkpoints = calloc(s.checkpoint_count + 1, sizeof *s.checkpoints);
	crs_ima_t state = { n ? samples[0] : 0, 20 };
	for (uint32_t i = 0; i < n; i++) {
		if (i % CHECKPOINT_INTERVAL == 0) {
			s.checkpoints[i / CHECKPOINT_INTERVAL].predictor = state.predictor;
			s.checkpoints[i / CHECKPOINT_INTERVAL].index = state.index;
		}
		int32_t sample = samples[i] > 32767 ? 32767 : samples[i] < -32768 ? -32768 : samples[i];
		int nibble = crs_ima_encode(&state, sample);
		s.nibbles[i / 2] |= nibble << (i & 1 ? 4 : 0);
		s.signal += (double)sample * sample;
		s.noise += (double)(sample - state.predictor) * (sample - state.predictor);
	}
	return s;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Every NAME.crm in dir, at rate Hz, as the "MUSI" section (crs_pack.h). */
static section_t make_music(const char *dir, uint32_t rate)
{
	section_t section = { "MUSI", NULL, 0 };
	char *names[64];
	int count = 0;
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && count < 64) {
		size_t length = strlen(e->d_name);
		if (length > 4 && length - 4 < 16 && !strcasecmp(e->d_name + length - 4, ".crm")) names[count++] = strdup(e->d_name);
	}
	if (d) closedir(d);
	qsort(names, count, sizeof names[0], by_name);

	crs_pack_song_t *songs = calloc(count, sizeof *songs);
	stream_t streams[64][2];
	uint32_t data_size = 0;
	double seconds = 0;

	for (int s = 0; s < count; s++) {
		char path[1200];
		snprintf(path, sizeof path, "%s/%s", dir, names[s]);
		FILE *f = fopen(path, "rb");
		uint8_t header[32];
		if (!f || fread(header, 1, 32, f) != 32 || memcmp(header, "CRSM", 4)) {
			fprintf(stderr, "make_crs: %s is not a .crm stream\n", path);
			exit(1);
		}
		uint32_t source_rate = be(header + 8, 4), frames[2] = { be(header + 12, 4), be(header + 16, 4) };
		uint32_t block = be(header + 20, 4);

		crs_pack_song_t *song = &songs[s];
		snprintf(song->name, sizeof song->name, "%.*s", (int)(strlen(names[s]) - 4), names[s]);
		song->rate = rate;
		song->level = be(header + 28, 2);
		song->fade_unit = be(header + 26, 2);
		for (int part = 0; part < 2; part++) {
			int32_t *mono = read_part(f, frames[part], block);
			if (!mono) {
				fprintf(stderr, "make_crs: %s is cut short\n", path);
				exit(1);
			}
			int32_t *resampled = resample(mono, frames[part], source_rate, rate, &song->samples[part]);
			streams[s][part] = encode(resampled, song->samples[part]);
			data_size += ((song->samples[part] + 1) / 2 + 3) & ~3u;
			data_size += streams[s][part].checkpoint_count * sizeof(crs_pack_checkpoint_t);
			free(mono);
			free(resampled);
		}
		fclose(f);

		double signal = streams[s][0].signal + streams[s][1].signal, noise = streams[s][0].noise + streams[s][1].noise;
		seconds += (double)(song->samples[0] + song->samples[1]) / rate;
		printf("  %-8s %6.1f s + %6.1f s, level %2u, ADPCM %4.1f dB\n", song->name, (double)song->samples[0] / rate,
		       (double)song->samples[1] / rate, song->level, noise > 0 ? 10 * log10(signal / noise) : 99.0);
	}

	/* Header, song table, then each song's streams and checkpoints. */
	uint32_t offset = sizeof(crs_pack_music_t) + count * sizeof(crs_pack_song_t);
	section.size = offset + data_size;
	section.data = calloc(1, section.size);
	crs_pack_music_t music = { count, CHECKPOINT_INTERVAL };
	for (int s = 0; s < count; s++) {
		for (int part = 0; part < 2; part++) {
			stream_t *st = &streams[s][part];
			uint32_t bytes = (songs[s].samples[part] + 1) / 2;
			songs[s].data[part] = offset;
			memcpy(section.data + offset, st->nibbles, bytes);
			offset += (bytes + 3) & ~3u;
			songs[s].checkpoints[part] = offset;
			memcpy(section.data + offset, st->checkpoints, st->checkpoint_count * sizeof(crs_pack_checkpoint_t));
			offset += st->checkpoint_count * sizeof(crs_pack_checkpoint_t);
			free(st->nibbles);
			free(st->checkpoints);
		}
	}
	memcpy(section.data, &music, sizeof music);
	memcpy(section.data + sizeof music, songs, count * sizeof *songs);
	printf("make_crs: music: %d songs, %.0f s at %u Hz, %u KB\n", count, seconds, rate, section.size / 1024);

	for (int s = 0; s < count; s++) free(names[s]);
	free(songs);
	return section;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage: make_crs image.bin data_dir music_dir work_dir out.crs [music_rate]\n");
		return 2;
	}
	uint32_t music_rate = argc > 6 ? (uint32_t)atoi(argv[6]) : 11025;

	crs_config_t config = { .image = argv[1], .data_dir = argv[2], .music_dir = argv[3], .save_dir = argv[4],
	                        .music = 1, .quiet = 1 };
	if (crs_init(&config)) return 1;

	/* The game once its files are loaded. */
	if (run_until(loaded)) {
		fprintf(stderr, "make_crs: the game did not reach frame %d\n", SNAPSHOT_FRAME);
		return 1;
	}
	if (crs_open_files()) {
		fprintf(stderr, "make_crs: files still open at frame %d\n", SNAPSHOT_FRAME);
		return 1;
	}
	int frame = crs_frame_count;

	uint32_t count = 0;
	for (uint32_t page = 0; page < CRS_MEM_SIZE / CRS_PACK_PAGE; page++) {
		const uint8_t *p = crs_page_read(page * CRS_PACK_PAGE);
		for (int i = 0; i < CRS_PACK_PAGE; i++) if (p[i]) { count++; break; }
	}
	section_t pages = { "PAGE", malloc(4 + count * 4), 4 + count * 4 };
	section_t data = { "PDAT", malloc(count * CRS_PACK_PAGE), count * CRS_PACK_PAGE };
	put32(pages.data, count);
	uint32_t n = 0;
	for (uint32_t page = 0; page < CRS_MEM_SIZE / CRS_PACK_PAGE; page++) {
		const uint8_t *p = crs_page_read(page * CRS_PACK_PAGE);
		int used = 0;
		for (int i = 0; i < CRS_PACK_PAGE; i++) if (p[i]) { used = 1; break; }
		if (!used) continue;
		put32(pages.data + 4 + n * 4, page);
		memcpy(data.data + n * CRS_PACK_PAGE, p, CRS_PACK_PAGE);
		n++;
	}

	uint32_t regs[CRS_CPU_REGS];
	crs_cpu_save(regs);
	section_t cpu = { "CPU ", malloc(CRS_CPU_REGS * 4), CRS_CPU_REGS * 4 };
	for (int i = 0; i < CRS_CPU_REGS; i++) put32(cpu.data + i * 4, regs[i]);

	section_t port = { "PORT", NULL, crs_state_save(NULL) };
	port.data = malloc(port.size);
	crs_state_save(port.data);

	/* The backgrounds, once the demo's game has built them. */
	if (run_until(built)) {
		fprintf(stderr, "make_crs: the game did not build GVRAM within %d frames\n", MAXIMUM_FRAMES);
		return 1;
	}
	section_t layers[2] = {
		{ "GFX0", malloc(CRS_PACK_LAYER_SIZE), CRS_PACK_LAYER_SIZE },
		{ "GFX1", malloc(CRS_PACK_LAYER_SIZE), CRS_PACK_LAYER_SIZE },
	};
	for (int l = 0; l < 2; l++)
		for (int i = 0; i < CRS_PACK_LAYER_SIZE; i++)
			layers[l].data[i] = crs_rd8(0xC00000 + l * 0x80000 + i * 2 + 1);	/* low byte of each word */

	section_t id = { "ID  ", malloc(4), 4 };
	put32(id.data, CRS_IMAGE_ID);

	/* The music, read from flash with the rest. */
	section_t music = make_music(argv[3], music_rate);

	/* The file: header, section table, sections at 4 KB boundaries. */
	section_t sections[] = { pages, data, cpu, port, layers[0], layers[1], id, music };
	const uint32_t section_count = sizeof sections / sizeof sections[0];
	uint32_t offset = CRS_PACK_ALIGN;
	crs_pack_header_t header = { { 'C', 'R', 'S', 'P' }, CRS_PACK_VERSION, section_count };
	crs_pack_section_t table[sizeof sections / sizeof sections[0]];
	for (uint32_t i = 0; i < section_count; i++) {
		memcpy(table[i].id, sections[i].id, 4);
		table[i].offset = offset;
		table[i].size = sections[i].size;
		offset = (offset + sections[i].size + CRS_PACK_ALIGN - 1) / CRS_PACK_ALIGN * CRS_PACK_ALIGN;
	}

	FILE *f = fopen(argv[5], "wb");
	if (!f) { perror(argv[5]); return 1; }
	uint8_t *file = calloc(1, offset);
	memcpy(file, &header, sizeof header);
	memcpy(file + sizeof header, table, sizeof table);
	for (uint32_t i = 0; i < section_count; i++) memcpy(file + table[i].offset, sections[i].data, sections[i].size);
	fwrite(file, 1, offset, f);
	fclose(f);

	printf("make_crs: %s, %u KB: state at frame %d (%u pages, %u KB), backgrounds built at frame %d\n",
	       argv[5], offset / 1024, frame, count, count * CRS_PACK_PAGE / 1024, crs_frame_count);
	return 0;
}
