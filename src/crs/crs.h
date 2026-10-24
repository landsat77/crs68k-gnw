/*
 * Cho Ren Sha 68k, portable port layer: the game core (sz2.s assembled for
 * the Amiga port, with game/host_stubs.s in place of the Amiga layer) runs
 * in Musashi; this layer does in C what the Amiga port's layer does in 68000
 * code: Human68k and IOCS calls, the frame waits and VBLs, the X68000 screen
 * (crs_render.c) and the sound (crs_audio.c).
 *
 * The 68000 memory has X68000 addresses (the image at $10000, the process
 * area at $200000, GVRAM at $C00000, text VRAM at $E00000, the I/O shadows
 * above), in 256-byte pages (crs_memory.c): read in place from a pack,
 * copied to RAM when written. Front ends: src/main.c (the Game & Watch
 * homebrew), src/pc/sdl_main.c (PC), tools/make_crs.c (packs).
 */

#ifndef CRS_H
#define CRS_H

#include <stdint.h>
#include <stdio.h>

#define CRS_MEM_SIZE 0x01000000
#define CRS_IMAGE_BASE 0x00010000u
#define CRS_HEAP_START 0x00200000u	/* process area: PSP, game stack and heap */
#define CRS_HEAP_SIZE 0x00200000u	/* as the Amiga build */

/* X68000 joystick bits (_JOYGET, active low). */
#define CRS_JOY_UP 0x01
#define CRS_JOY_DOWN 0x02
#define CRS_JOY_LEFT 0x04
#define CRS_JOY_RIGHT 0x08
#define CRS_JOY_A 0x20
#define CRS_JOY_B 0x40

/* Buttons the front end sets (active high). */
typedef struct {
	uint8_t joystick;	/* CRS_JOY_* */
	uint8_t start;		/* the X68000's ESC: pause, back in menus */
	uint8_t autofire;	/* trigger A held fires repeatedly in a stage */
} crs_input_t;

typedef struct {
	const uint8_t *pack;	/* a .crs pack (crs_pack.h) to start from, or NULL */
	uint32_t pack_size;
	const char *image;	/* else the linked game (vlink -brawbin1 at $10000) */
	const char *data_dir;	/* BGM_DAT, ETC_DAT, PCM_DAT, XSP_DAT, MUSIC_DAT (not
				   read when starting from a pack) */
	const char *save_dir;	/* files the game writes (high scores, settings) */
	const char *music_dir;	/* the .crm streams (NULL: data_dir/MUSIC_DAT) */
	int music;		/* play the MUSIC_DAT .crm streams when present */
	int quiet;		/* no PRINT/PRINTF output */
	uint8_t *ram;		/* RAM for the written pages and their tables, or */
	uint32_t ram_size;	/* NULL: enough for all 16 MB is allocated */
	void (*tick)(void);	/* called between slices of 68000 time (watchdog) */
} crs_config_t;

/* Messages: standard error on the PC, the firmware's log on the device. */
#ifdef CRS_GNW
#define CRS_LOG(...) printf(__VA_ARGS__)
#else
#define CRS_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

/* Result of crs_run_frame. */
#define CRS_EXITED -1

extern crs_input_t crs_input;
extern int crs_frame_count;	/* frame waits so far */
extern long long crs_vbl_count;	/* VBLs run so far (55.46 Hz) */
extern int crs_graphics_builds;	/* times the game has built GVRAM */

/* GVRAM pages 0 and 1 as 512 x 512 graphics palette indices, from a pack:
   the renderer draws them instead of GVRAM. NULL: GVRAM. */
extern const uint8_t *crs_layers[2];

/* The sprites XSP_OUT passed last (8 bytes each: x, y, pattern, attributes,
   big-endian words as in 68000 memory). */
#define CRS_MAXIMUM_SPRITES 512
extern uint8_t crs_sprite_list[CRS_MAXIMUM_SPRITES * 8];
extern int crs_sprite_count;
extern int crs_frame_sprite_count;	/* passed during the last frame (checkpoints) */

int crs_init(const crs_config_t *config);

/* Runs the game until it ends a frame (a frame wait, after which the
   picture is complete) and returns the number of VBLs that frame lasts
   (0 or more; 55.46 per second), or CRS_EXITED. A game that polls the
   joystick for a long time without ending a frame (the pause) also returns,
   with 1, so the front end keeps showing the screen and reading input. */
int crs_run_frame(void);

/* Game state for comparisons with tests/amiga/reference/attract-6000.txt
   (one line; the reference has one every 60 frames). */
void crs_write_checkpoint(FILE *f);

/* For make_crs: the CPU registers and the port layer's state between two
   frames (with the memory, the whole state of the game). crs_state_save
   returns the size; out NULL only measures. */
#define CRS_CPU_REGS 26
void crs_cpu_save(uint32_t regs[CRS_CPU_REGS]);
uint32_t crs_state_save(uint8_t *out);
int crs_open_files(void);

/* crs_audio.c's part of the state. */
uint32_t crs_audio_state_save(uint8_t *out);
const uint8_t *crs_audio_state_load(const uint8_t *in);
#define CRS_AUDIO_PLAY_SIZE 20
void crs_audio_play_save(uint8_t *out);
void crs_audio_play_load(const uint8_t *in);

/* Save states: the whole game (registers, port layer, sound playing, the
   pages that differ from the pack) to a file, and back. 0 on success. */
int crs_save_state(FILE *f);
int crs_load_state(FILE *f);
int crs_memory_save(FILE *f);
int crs_memory_load(FILE *f);

/* crs_memory.c: the 68000 memory in 256-byte pages. crs_page_read gives the
   page holding an address (zeros if never written), crs_page_write the same
   page in RAM (copied there first if needed; NULL in a sink range). */
void crs_memory_init(uint8_t *ram, uint32_t size);
void crs_memory_map(const uint8_t *numbers, const uint8_t *data, uint32_t count);
void crs_memory_sink(uint32_t start, uint32_t end);
void crs_memory_release(uint32_t start, uint32_t end);
const uint8_t *crs_page_read(uint32_t address);
uint8_t *crs_page_write(uint32_t address);
extern uint32_t crs_ram_pages, crs_ram_pages_peak;	/* pages in RAM now, at most */
extern uint32_t crs_ram_tables;	/* segment tables (1 KB each) */
extern const uint8_t crs_zero_page[];	/* what crs_page_read gives for unwritten pages */
extern void (*crs_fatal)(const char *message);	/* out of RAM; then abort() */

/* Big-endian access (not traced). */
uint32_t crs_rd8(uint32_t a);
uint32_t crs_rd16(uint32_t a);
uint32_t crs_rd32(uint32_t a);
void crs_wr8(uint32_t a, uint32_t v);
void crs_wr16(uint32_t a, uint32_t v);
void crs_wr32(uint32_t a, uint32_t v);
void crs_read_bytes(uint32_t a, void *out, uint32_t n);
void crs_write_bytes(uint32_t a, const void *in, uint32_t n);

/* crs_render.c: lines first to first + count - 1 of the visible 256 x 256
   X68000 screen as RGB565 (stride in pixels). */
#define CRS_SCREEN_WIDTH 256
#define CRS_SCREEN_HEIGHT 256
void crs_render(uint16_t *out, int stride, int first, int count);

/* crs_audio.c: mono 16-bit samples at the given rate (the front end's
   audio callback). The front end sets crs_audio_lock (lock 1 / unlock 0)
   when it mixes from another thread. */
extern void (*crs_audio_lock)(int lock);
void crs_audio_init(const char *music_dir, int music);
void crs_audio_mix(int16_t *out, int samples, int rate);
int crs_audio_music_available(void);
void crs_audio_set_music(int on);
/* The pack's "MUSI" section (crs_core.c, when it loads a pack; NULL: none):
   its songs play from it in place of the .crm files. */
void crs_audio_pack_music(const uint8_t *section, uint32_t size);
/* From the game (crs_core.c). */
void crs_audio_effect(uint32_t address, uint32_t length, int rate_index, int channels);
void crs_audio_effect_stop(void);
int crs_audio_effect_playing(void);
void crs_audio_song_loaded(uint32_t data, const char *name);
void crs_audio_music_call(uint32_t function, uint32_t d1, uint32_t a0);
void crs_audio_vbl(void);

/* Optional hooks (memtrace): defined by the front end when CRS_TRACE is set. */
#ifdef CRS_TRACE
#define CRS_TRACE_READ 0
#define CRS_TRACE_WRITE 1
#define CRS_TRACE_EXEC 2
#define CRS_TRACE_FILE 3
void crs_trace(int kind, uint32_t address, uint32_t length);
void crs_trace_load(const char *name, uint32_t address, uint32_t length);
#endif

#endif
