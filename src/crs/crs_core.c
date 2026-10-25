/*
 * The game core in Musashi, with the Human68k, IOCS and frame handling of
 * the Amiga port's emulator.s (see crs.h). The Amiga hooks in sz2.s land in
 * game/host_stubs.s; the instruction hook below does their work when
 * the PC reaches them, and handles line F (DOS), trap #15 (IOCS), trap #2
 * (PCM8) and trap #4 (MCDRV) in place.
 *
 * VBLs run in lockstep with the game's frame waits (as the Amiga build's
 * __LOCKSTEP__ mode): runs are deterministic, and the front end paces the
 * frames at 55.46 Hz.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#ifndef CRS_GNW
#include <dirent.h>
#endif
#include "m68k.h"
#include "crs.h"
#include "crs_pack.h"
#include "crs_symbols.h"

#define DUMMY_RTE 0x00BF0000u	/* initial VBL and raster vectors */

/* Instructions without a frame end after which a _JOYGET returns a poll
   frame (the pause loops until START without ending frames). */
#define POLL_INSTRUCTIONS 200000

crs_input_t crs_input;
int crs_frame_count;
long long crs_vbl_count;
int crs_graphics_builds;
const uint8_t *crs_layers[2];
uint8_t crs_sprite_list[CRS_MAXIMUM_SPRITES * 8];
int crs_sprite_count;
int crs_frame_sprite_count;

static crs_config_t config;
static int frame_done, exited;
static int frame_vbls;		/* VBLs run during the current frame */
static int sprites_this_frame;
static long instructions_since_frame;
static int stage_frame, last_stage_frame;	/* the game ran its ship control */
static int autofire_phase;
static long long effect_end_vbl;

/* --- memory --- */

#ifdef CRS_TRACE
#define TRACE(kind, a, n) crs_trace(kind, a, n)
#else
#define TRACE(kind, a, n)
#endif

static inline uint32_t mem_read(uint32_t a, int n)
{
	return n == 1 ? crs_rd8(a) : n == 2 ? crs_rd16(a) : crs_rd32(a);
}

static inline void mem_write(uint32_t a, uint32_t v, int n)
{
	a &= 0xFFFFFF;
	TRACE(CRS_TRACE_WRITE, a, n);
	if (n == 1) crs_wr8(a, v);
	else if (n == 2) crs_wr16(a, v);
	else crs_wr32(a, v);
}

unsigned int m68k_read_memory_8(unsigned int a) { TRACE(CRS_TRACE_READ, a & 0xFFFFFF, 1); return mem_read(a, 1); }
unsigned int m68k_read_memory_16(unsigned int a) { TRACE(CRS_TRACE_READ, a & 0xFFFFFF, 2); return mem_read(a, 2); }
unsigned int m68k_read_memory_32(unsigned int a) { TRACE(CRS_TRACE_READ, a & 0xFFFFFF, 4); return mem_read(a, 4); }
unsigned int m68k_read_immediate_16(unsigned int a) { TRACE(CRS_TRACE_EXEC, a & 0xFFFFFF, 2); return mem_read(a, 2); }
unsigned int m68k_read_immediate_32(unsigned int a) { TRACE(CRS_TRACE_EXEC, a & 0xFFFFFF, 4); return mem_read(a, 4); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { mem_write(a, v, 1); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { mem_write(a, v, 2); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { mem_write(a, v, 4); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return mem_read(a, 1); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return mem_read(a, 2); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return mem_read(a, 4); }

/* Writes by the port layer count as the game's (memtrace). */
static void host_write16(uint32_t a, uint32_t v) { mem_write(a, v, 2); }
static void host_write32(uint32_t a, uint32_t v) { mem_write(a, v, 4); }

static void game_string(uint32_t a, char *out, int size)
{
	int i = 0;
	while (crs_rd8(a) && i < size - 1) out[i++] = crs_rd8(a++);
	out[i] = 0;
}

/* --- files --- */

#define FIRST_FILE_HANDLE 5	/* 0-4 are the standard handles */
#define NUMBER_OF_FILE_HANDLES 16

static FILE *files[NUMBER_OF_FILE_HANDLES];
static char file_names[NUMBER_OF_FILE_HANDLES][64];

/* The game's path ("XSP_DAT\SZ2.XSP") under dir, each component matched
   without regard to case (the Amiga and Human68k file systems ignore it;
   so does FatFs on the Game & Watch, CRS_GNW, where the path is just
   joined). */
static int find_path(const char *dir, const char *game_path, char *out, int size)
{
#ifdef CRS_GNW
	const char *p = game_path;
	if (p[0] && p[1] == ':') p += 2;
	while (*p == '\\' || *p == '/') p++;
	int n = snprintf(out, size, "%s/%s", dir, p);
	for (int i = 0; i < n && i < size; i++) if (out[i] == '\\') out[i] = '/';
	return 1;
#else
	char path[1024];
	snprintf(path, sizeof path, "%s", dir);

	const char *p = game_path;
	if (p[0] && p[1] == ':') p += 2;

	while (*p) {
		while (*p == '\\' || *p == '/') p++;
		if (!*p) break;

		char component[256];
		int n = 0;
		while (*p && *p != '\\' && *p != '/' && n < 255) component[n++] = *p++;
		component[n] = 0;

		DIR *d = opendir(path);
		if (!d) return 0;
		struct dirent *e;
		int found = 0;
		while ((e = readdir(d))) {
			if (!strcasecmp(e->d_name, component)) {
				size_t length = strlen(path);
				snprintf(path + length, sizeof path - length, "/%s", e->d_name);
				found = 1;
				break;
			}
		}
		closedir(d);
		if (!found) return 0;
	}

	snprintf(out, size, "%s", path);
	return 1;
#endif
}

/* Files the game writes go to save_dir (upper-case base name); reads look
   there first, then in data_dir. */
static FILE *open_game_file(const char *game_path, int create, int mode)
{
	char path[1024], saved[1024];
	const char *base = game_path + strlen(game_path);
	while (base > game_path && base[-1] != '\\' && base[-1] != '/' && base[-1] != ':') base--;
	snprintf(saved, sizeof saved, "%s/", config.save_dir);
	size_t prefix = strlen(saved);
	for (const char *c = base; *c && prefix < sizeof saved - 1; c++) saved[prefix++] = toupper((unsigned char)*c);
	saved[prefix] = 0;

	if (create) return fopen(saved, "w+b");

	FILE *f = fopen(saved, mode ? "r+b" : "rb");
	if (f) return f;

	if (!config.data_dir || !find_path(config.data_dir, game_path, path, sizeof path)) return NULL;
	if (!mode) return fopen(path, "rb");

	/* Opened for writing: work on a copy in save_dir. */
	FILE *source = fopen(path, "rb");
	FILE *copy = fopen(saved, "w+b");
	if (!source || !copy) {
		if (source) fclose(source);
		if (copy) fclose(copy);
		return NULL;
	}
	int c;
	while ((c = fgetc(source)) != EOF) fputc(c, copy);
	fclose(source);
	rewind(copy);
	return copy;
}

static FILE *file_for(uint32_t handle)
{
	int i = (int16_t)handle - FIRST_FILE_HANDLE;
	return i >= 0 && i < NUMBER_OF_FILE_HANDLES ? files[i] : NULL;
}

/* --- Human68k --- */

/* _NAMECK(name, buffer): drive (2), path (65), name (19), extension (5). */
static void nameck(uint32_t name, uint32_t buffer)
{
	char s[256], path[80] = "\\", file[20] = "", extension[6] = "";
	game_string(name, s, sizeof s);
	char *p = s, drive = 'A';
	if (p[0] && p[1] == ':') drive = p[0], p += 2;

	char *base = p;
	for (char *c = p; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;

	int n = 1;
	for (char *c = p; c < base && n < 64; c++) {
		char ch = *c == '/' ? '\\' : *c;
		if (ch == '\\' && path[n - 1] == '\\') continue;
		path[n++] = ch;
	}
	if (path[n - 1] != '\\') path[n++] = '\\';
	path[n] = 0;

	char *dot = strchr(base, '.');
	int length = dot ? (int)(dot - base) : (int)strlen(base);
	snprintf(file, sizeof file, "%.*s", length > 18 ? 18 : length, base);
	if (dot) snprintf(extension, sizeof extension, "%.4s", dot);

	for (int i = 0; i < 91; i++) mem_write(buffer + i, 0, 1);
	mem_write(buffer, drive, 1);
	mem_write(buffer + 1, ':', 1);
	for (int i = 0; path[i]; i++) mem_write(buffer + 2 + i, path[i], 1);
	for (int i = 0; file[i]; i++) mem_write(buffer + 67 + i, file[i], 1);
	for (int i = 0; extension[i]; i++) mem_write(buffer + 86 + i, extension[i], 1);
}

static void stop(void)
{
	exited = 1;
	m68k_end_timeslice();
}

/* Line F call $FFxx (DOS) or $FExx (FLOAT), as number $0xx / $1xx:
   arguments on the stack as at the instruction. */
static void dos_call(int number)
{
	uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
	int32_t d0 = 0;

	switch (number) {
	case 0x100: /* __LMUL */
		d0 = (int32_t)m68k_get_reg(NULL, M68K_REG_D0) * (int32_t)m68k_get_reg(NULL, M68K_REG_D1);
		break;
	case 0x101: { /* __LDIV */
		int32_t divisor = m68k_get_reg(NULL, M68K_REG_D1);
		d0 = divisor ? (int32_t)m68k_get_reg(NULL, M68K_REG_D0) / divisor : 0;
		break;
	}
	case 0x10D: /* __SRAND */
	case 0x00D: /* _FFLUSH */
	case 0x020: /* _SUPER */
	case 0x021: /* _FNCKEY */
	case 0x023: /* _CONCTRL */
	case 0x025: /* _INTVCS */
	case 0x044: /* _IOCTRL */
		break;
	case 0x006: /* _INPOUT: $FE/$FF read a key without waiting; any key is RETURN */
		if ((crs_rd16(sp) & 0xFF) >= 0xFE && (crs_input.start || (crs_input.joystick & (CRS_JOY_A | CRS_JOY_B))))
			d0 = 0x0D;
		break;
	case 0x000: /* _EXIT */
	case 0x04C: /* _EXIT2 */
		stop();
		break;
	case 0x009: { /* _PRINT */
		char s[256];
		game_string(crs_rd32(sp), s, sizeof s);
		if (!config.quiet) CRS_LOG("%s", s);
		break;
	}
	case 0x036: /* _DSKFRE */
		d0 = 10000000;
		break;
	case 0x037: /* _NAMECK */
		nameck(crs_rd32(sp), crs_rd32(sp + 4));
		break;
	case 0x03C: /* _CREATE */
	case 0x03D: { /* _OPEN */
		char name[256];
		game_string(crs_rd32(sp), name, sizeof name);
		int slot = -1;
		for (int i = 0; i < NUMBER_OF_FILE_HANDLES; i++) if (!files[i]) { slot = i; break; }
		if (slot < 0) { d0 = -4; break; }
		FILE *f = open_game_file(name, number == 0x03C, number == 0x03D ? crs_rd16(sp + 4) & 3 : 2);
		if (!f) { d0 = -2; break; }
		files[slot] = f;
		snprintf(file_names[slot], sizeof file_names[slot], "%.63s", name);
		d0 = slot + FIRST_FILE_HANDLE;
		break;
	}
	case 0x03E: { /* _CLOSE */
		FILE *f = file_for(crs_rd16(sp));
		if (!f) { d0 = -6; break; }
		fclose(f);
		files[(int16_t)crs_rd16(sp) - FIRST_FILE_HANDLE] = NULL;
		break;
	}
	case 0x03F: { /* _READ(handle, buffer, length) */
		FILE *f = file_for(crs_rd16(sp));
		if (!f) { d0 = -6; break; }
		uint32_t buffer = crs_rd32(sp + 2) & 0xFFFFFF, length = crs_rd32(sp + 6) & 0x7FFFFFFF;
		if (buffer + length > CRS_MEM_SIZE) length = CRS_MEM_SIZE - buffer;
		d0 = 0;
		for (uint32_t done = 0; done < length;) {
			uint8_t chunk[4096];
			uint32_t want = length - done < sizeof chunk ? length - done : sizeof chunk;
			size_t got = fread(chunk, 1, want, f);
			crs_write_bytes(buffer + done, chunk, got);
			done += got;
			d0 = done;
			if (got < want) break;
		}
#ifdef CRS_TRACE
		if (d0 > 0) {
			crs_trace(CRS_TRACE_FILE, buffer, d0);
			crs_trace_load(file_names[(int16_t)crs_rd16(sp) - FIRST_FILE_HANDLE], buffer, d0);
		}
#endif
		break;
	}
	case 0x040: { /* _WRITE(handle, buffer, length) */
		FILE *f = file_for(crs_rd16(sp));
		if (!f) { d0 = -6; break; }
		uint32_t buffer = crs_rd32(sp + 2) & 0xFFFFFF, length = crs_rd32(sp + 6) & 0x7FFFFFFF;
		if (buffer + length > CRS_MEM_SIZE) length = CRS_MEM_SIZE - buffer;
		d0 = 0;
		for (uint32_t done = 0; done < length;) {
			uint8_t chunk[4096];
			uint32_t n = length - done < sizeof chunk ? length - done : sizeof chunk;
			crs_read_bytes(buffer + done, chunk, n);
			d0 += fwrite(chunk, 1, n, f);
			done += n;
		}
#ifndef CRS_GNW
		fflush(f);	/* the firmware's files have no fflush; written when closed */
#endif
		break;
	}
	case 0x041: /* _DELETE: only the copies in save_dir are ever written */
		break;
	case 0x042: { /* _SEEK(handle, offset, mode): returns the new position */
		FILE *f = file_for(crs_rd16(sp));
		if (!f) { d0 = -6; break; }
		static const int whence[] = { SEEK_SET, SEEK_CUR, SEEK_END, SEEK_SET };
		d0 = fseek(f, (int32_t)crs_rd32(sp + 2), whence[crs_rd16(sp + 6) & 3]) ? -1 : ftell(f);
		break;
	}
	case 0x04A: { /* _SETBLOCK(block, size) */
		uint32_t largest = CRS_HEAP_START + CRS_HEAP_SIZE - crs_rd32(sp);
		d0 = crs_rd32(sp + 4) > largest ? (int32_t)(largest | 0x81000000u) : 0;
		break;
	}
	case 0x051: /* _GETPDB */
		d0 = CRS_HEAP_START + 16;
		break;
	default:
		CRS_LOG("unimplemented DOS call $%s%02X\n", number & 0x100 ? "FE" : "FF", number & 0xFF);
		d0 = -1;
		break;
	}

	m68k_set_reg(M68K_REG_D0, d0);
}

/* The joystick byte (active low) as the game reads it: START is also left +
   right (the game's pad setting "START SELECT ON"); a held trigger A reads
   released every other 2 frames in a stage (auto fire). */
static uint32_t joystick_state(void)
{
	uint8_t pressed = crs_input.joystick;
	if (crs_input.autofire && last_stage_frame && (pressed & CRS_JOY_A) && (autofire_phase & 2))
		pressed &= ~CRS_JOY_A;
	return ~pressed & 0xFF;
}

/* IOCS (trap #15), d0.b = function: as iocs_call in emulator.s. */
static void iocs_call(void)
{
	uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0) & 0xFF;
	uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
	uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
	uint32_t d3 = m68k_get_reg(NULL, M68K_REG_D3);
	uint32_t result = 0;

	switch (d0) {
	case 0x04: /* _BITSNS: group 0 bit 1 = ESC (START) */
		if ((d1 & 0xF) == 0 && crs_input.start) result = 0x02;
		break;
	case 0x14: { /* _TPALET2 */
		uint32_t a = 0xE82200 + (d1 & 0xF) * 2;
		if ((int32_t)d2 < 0) result = crs_rd16(a);
		else host_write16(a, d2);
		break;
	}
	case 0x3B: /* _JOYGET */
		result = joystick_state();
		if (instructions_since_frame > POLL_INSTRUCTIONS) {
			/* Polling without frames (the pause): let the front end show
			   the screen and read the input. */
			instructions_since_frame = 0;
			frame_vbls = 1;
			frame_done = 1;
			m68k_end_timeslice();
		}
		break;
	case 0x60: /* _ADPCMOUT: a1 = data, d1 = rate << 8 | outputs, d2 = length */
		if (d1 & 3) {
			int rate = (d1 >> 8) & 0xFF;
			if (rate > 4) rate = 4;
			static const double rates[] = { 3906.25, 5208.33, 7812.5, 10416.67, 15625.0 };
			/* The end as emulator.s expects it, for _ADPCMSNS. */
			effect_end_vbl = crs_vbl_count + (long long)(d2 * 55.46 / rates[rate]) + 1;
			crs_audio_effect(m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF, d2, rate, d1 & 3);
		}
		break;
	case 0x66: /* _ADPCMSNS: 2 while an effect plays */
		result = crs_vbl_count < effect_end_vbl ? 2 : 0;
		break;
	case 0x67: /* _ADPCMMOD: 0 stops */
		if (!d1) {
			effect_end_vbl = 0;
			crs_audio_effect_stop();
		}
		break;
	case 0x7F: /* _ONTIME: fixed, as the ports (replays stay deterministic) */
		result = 100 * 100;
		m68k_set_reg(M68K_REG_D1, 0);
		break;
	case 0x81: /* _B_SUPER: already supervisor */
		result = (uint32_t)-1;
		break;
	case 0x92: /* _VC_R1: d1.w = new R1, -1 reads it; returns the old value */
		result = crs_rd16(0xE82500);
		if ((d1 & 0xFFFF) != 0xFFFF) host_write16(0xE82500, d1);
		break;
	case 0xCE: { /* _BGTEXTGT: d1 = page, d2 = x, d3 = y */
		uint32_t a = 0xEBC000 + (d1 & 1) * 0x2000 + (((d3 & 63) << 6) + (d2 & 63)) * 2;
		TRACE(CRS_TRACE_READ, a, 2);
		result = crs_rd16(a);
		break;
	}
	case 0xCF: { /* _SPALET: d1 = colour, d2 = palette block, d3 = value or < 0 */
		uint32_t a = 0xE82200 + (((d2 & 0xFFFF) << 4) + (d1 & 0xFFFF)) * 2;
		if ((int32_t)d3 < 0) result = crs_rd16(a);
		else host_write16(a, d3);
		break;
	}
	case 0xB5: /* _WIPE: the graphics screen cleared (the menus, after a game) */
		crs_bg_wipe();
		break;
	case 0x10: case 0x20: case 0x22: case 0x23: case 0x7D: case 0x87: case 0x90:
	case 0xAE: case 0xAF: case 0xB1: case 0xB2: case 0xB3: case 0xB4:
	case 0xC1: case 0xC2: case 0xCA:
		break;
	default:
		CRS_LOG("unimplemented IOCS call $%02X\n", (unsigned)d0);
		break;
	}

	m68k_set_reg(M68K_REG_D0, result);
}

/* --- frames --- */

static void frame_end(void)
{
	/* As frame_wait_begin: the sprites XSP_OUT passed during this frame. */
	crs_frame_sprite_count = sprites_this_frame;
	sprites_this_frame = 0;
	last_stage_frame = stage_frame;
	stage_frame = 0;
	autofire_phase++;
	instructions_since_frame = 0;
	crs_frame_count++;
	frame_done = 1;
	m68k_end_timeslice();
}

/* XSP_OUT: keeps the sprite list for the renderer and resets the game's. */
static void prepare_sprites(void)
{
	uint32_t n = (crs_rd32(CRS_SYM_CURRENT_SPRITE_DATA_ENTRY) - CRS_SYM_SPRITE_DATA_TABLE) / 8;
	if (n > CRS_MAXIMUM_SPRITES) n = CRS_MAXIMUM_SPRITES;
	TRACE(CRS_TRACE_READ, CRS_SYM_SPRITE_DATA_TABLE, n * 8);
	crs_read_bytes(CRS_SYM_SPRITE_DATA_TABLE, crs_sprite_list, n * 8);
	crs_sprite_count = n;
	sprites_this_frame = n;
	host_write32(CRS_SYM_CURRENT_SPRITE_DATA_ENTRY, CRS_SYM_SPRITE_DATA_TABLE);
}

void instruction_hook(unsigned int pc)
{
	pc &= 0xFFFFFF;
	instructions_since_frame++;

	if (pc >= CRS_SYM_host_stubs_start && pc < CRS_SYM_host_stubs_end) {
		switch (pc) {
		case CRS_SYM_host_frame_end:
			frame_end();
			break;
		case CRS_SYM_host_vbl:
			crs_vbl_count++;
			frame_vbls++;
			crs_audio_vbl();
			crs_bg_vbl();
			break;
		case CRS_SYM_amiga_exit_game:
			stop();
			break;
		case CRS_SYM_prepare_sprite_infos:
			prepare_sprites();
			break;
		case CRS_SYM_amiga_ship_control:
			stage_frame = 1;
			break;
		case CRS_SYM_amiga_graphics_changed:
			/* PLAY_GAME has built GVRAM: a game starts (stage in the
			   score's fifth byte, as the stage dispatch reads it). */
			crs_graphics_builds++;
			crs_bg_game_start(crs_rd16(CRS_SYM_PLAYER_SCORE + 4));
			break;
		case CRS_SYM_host_bg_frame:
			crs_bg_frame();
			break;
		case CRS_SYM_host_bg_script:
			crs_bg_script(crs_rd16(CRS_SYM_PLAYER_SCORE + 4), crs_rd16(CRS_SYM_WORD_000A8EEA),
			              (int16_t)crs_rd16(CRS_SYM_WORD_000A8EEC));
			break;
		case CRS_SYM_amiga_clear_text_planes:
			/* Not traced: a port keeps the text layer its own way. Its
			   pages go back to the pool. */
			crs_memory_release(0xE00000, 0xE80000);
			break;
		case CRS_SYM_amiga_mcdrv_probe:
			m68k_set_reg(M68K_REG_D0, crs_audio_music_available() ? 0x69 : (uint32_t)-1);
			break;
		case CRS_SYM_amiga_music_loaded: {
			char name[256];
			game_string(m68k_get_reg(NULL, M68K_REG_A1), name, sizeof name);
			crs_audio_song_loaded(m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF, name);
			break;
		}
		case CRS_SYM_amiga_printf:
			if (!config.quiet) {
				char s[256];
				game_string(crs_rd32(m68k_get_reg(NULL, M68K_REG_A7) + 4), s, sizeof s);
				CRS_LOG("PRINTF: %s\n", s);
			}
			break;
		}
		return;
	}

	uint32_t op = crs_rd16(pc);

	if ((op & 0xFE00) == 0xFE00) {
		dos_call((op & 0x1FF) ^ 0x100);	/* $FFxx -> $0xx, $FExx -> $1xx */
		m68k_set_reg(M68K_REG_PC, pc + 2);
	} else if (op == 0x4E4F) {
		iocs_call();
		m68k_set_reg(M68K_REG_PC, pc + 2);
	} else if (op == 0x4E44) {
		crs_audio_music_call(m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
		                     m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF);
		m68k_set_reg(M68K_REG_D0, 0);
		m68k_set_reg(M68K_REG_PC, pc + 2);
	} else if (op == 0x4E42) {
		m68k_set_reg(M68K_REG_PC, pc + 2);	/* PCM8: the game only reads its mode */
	}
}

/* One line of tests/amiga/reference/attract-6000.txt: frame, score, random
   table index, background scroll counter, sprites, player structure. */
void crs_write_checkpoint(FILE *f)
{
	fprintf(f, "%d %u %u %u %d ", crs_frame_count, (unsigned)crs_rd32(CRS_SYM_PLAYER_SCORE),
	        (unsigned)crs_rd16(CRS_SYM_WORD_00088E6C), (unsigned)crs_rd16(CRS_SYM_BACKGROUND_SCROLL_COUNTER),
	        crs_frame_sprite_count);
	for (int i = 0; i < 14; i++) fprintf(f, "%02x", (unsigned)crs_rd8(CRS_SYM_PLAYER_INFO_STRUCT + i));
	fputc('\n', f);
}

int crs_run_frame(void)
{
	frame_done = 0;
	frame_vbls = 0;
	while (!frame_done && !exited) {
		m68k_execute(400000);	/* about 100,000 instructions */
		if (config.tick) config.tick();
	}
	return exited ? CRS_EXITED : frame_vbls;
}

/* --- state (make_crs, packs) --- */

/* CRS_CPU_REGS registers, in this order. */
static const int cpu_regs[CRS_CPU_REGS] = {
	M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3, M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
	M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3, M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7,
	M68K_REG_PC, M68K_REG_SR, M68K_REG_USP, M68K_REG_ISP, M68K_REG_MSP, M68K_REG_SFC, M68K_REG_DFC,
	M68K_REG_VBR, M68K_REG_CACR, M68K_REG_CAAR,
};

void crs_cpu_save(uint32_t regs[CRS_CPU_REGS])
{
	for (int i = 0; i < CRS_CPU_REGS; i++) regs[i] = m68k_get_reg(NULL, cpu_regs[i]);
}

/* SR first (it selects the stack pointer A7 is), then the stack pointers,
   then the rest. */
static void cpu_load(const uint32_t regs[CRS_CPU_REGS])
{
	m68k_set_reg(M68K_REG_SR, regs[17]);
	for (int i = 18; i < 21; i++) m68k_set_reg(cpu_regs[i], regs[i]);
	for (int i = 0; i < CRS_CPU_REGS; i++) if (i != 17) m68k_set_reg(cpu_regs[i], regs[i]);
}

int crs_open_files(void)
{
	int n = 0;
	for (int i = 0; i < NUMBER_OF_FILE_HANDLES; i++) n += files[i] != NULL;
	return n;
}

/* Little-endian fields, in this order; crs_state_load reads them back. */
static uint8_t *put32(uint8_t *p, uint32_t v)
{
	if (p) for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
	return p ? p + 4 : NULL;
}

static uint32_t get32(const uint8_t **p)
{
	uint32_t v = (*p)[0] | (*p)[1] << 8 | (*p)[2] << 16 | (uint32_t)(*p)[3] << 24;
	*p += 4;
	return v;
}

uint32_t crs_state_save(uint8_t *out)
{
	const uint32_t fields[] = {
		crs_frame_count, (uint32_t)crs_vbl_count, (uint32_t)(crs_vbl_count >> 32),
		crs_sprite_count, crs_frame_sprite_count, sprites_this_frame, stage_frame, last_stage_frame,
		autofire_phase, (uint32_t)effect_end_vbl, (uint32_t)(effect_end_vbl >> 32),
		(uint32_t)instructions_since_frame, crs_graphics_builds,
	};
	uint32_t size = 0;
	uint8_t *p = out;
	for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++) p = put32(p, fields[i]), size += 4;
	if (p) memcpy(p, crs_sprite_list, sizeof crs_sprite_list), p += sizeof crs_sprite_list;
	size += sizeof crs_sprite_list;
	uint32_t audio = crs_audio_state_save(p);
	return size + audio + crs_bg_state_save(p ? p + audio : NULL);
}

static void state_load(const uint8_t *p)
{
	crs_frame_count = get32(&p);
	crs_vbl_count = get32(&p);
	crs_vbl_count |= (long long)get32(&p) << 32;
	crs_sprite_count = get32(&p);
	crs_frame_sprite_count = get32(&p);
	sprites_this_frame = get32(&p);
	stage_frame = get32(&p);
	last_stage_frame = get32(&p);
	autofire_phase = get32(&p);
	effect_end_vbl = get32(&p);
	effect_end_vbl |= (long long)get32(&p) << 32;
	instructions_since_frame = get32(&p);
	crs_graphics_builds = get32(&p);
	memcpy(crs_sprite_list, p, sizeof crs_sprite_list);
	crs_bg_state_load(crs_audio_state_load(p + sizeof crs_sprite_list));
}

/* The game as the pack holds it: its pages (the rest of memory is zero),
   the backgrounds, the registers and the port layer's state. */
static int load_pack(void)
{
	uint32_t pages_size = 0, data_size = 0, cpu_size = 0, port_size = 0, layer_size[2] = { 0, 0 };
	const uint8_t *pages = crs_pack_section(config.pack, config.pack_size, "PAGE", &pages_size);
	const uint8_t *data = crs_pack_section(config.pack, config.pack_size, "PDAT", &data_size);
	const uint8_t *cpu = crs_pack_section(config.pack, config.pack_size, "CPU ", &cpu_size);
	const uint8_t *port = crs_pack_section(config.pack, config.pack_size, "PORT", &port_size);
	crs_layers[0] = crs_pack_section(config.pack, config.pack_size, "GFX0", &layer_size[0]);
	crs_layers[1] = crs_pack_section(config.pack, config.pack_size, "GFX1", &layer_size[1]);
	if (!pages || !data || !cpu || !port || !crs_layers[0] || !crs_layers[1]
	    || cpu_size != CRS_CPU_REGS * 4 || layer_size[0] != CRS_PACK_LAYER_SIZE || layer_size[1] != CRS_PACK_LAYER_SIZE) {
		CRS_LOG("not a .crs pack (version %d)\n", CRS_PACK_VERSION);
		return -1;
	}

	/* Its pages are read where they are; GVRAM is the layers. */
	uint32_t id_size;
	uint32_t music_size = 0;
	const uint8_t *music = crs_pack_section(config.pack, config.pack_size, "MUSI", &music_size);
	crs_audio_pack_music(music, music_size);
	uint32_t bg_size = 0;
	const uint8_t *bg = crs_pack_section(config.pack, config.pack_size, "BG11", &bg_size);
	crs_bg_set_data(bg, bg_size);

	const uint8_t *id = crs_pack_section(config.pack, config.pack_size, "ID  ", &id_size);
	if (!id || id_size != 4 || (id[0] | id[1] << 8 | id[2] << 16 | (uint32_t)id[3] << 24) != CRS_IMAGE_ID) {
		CRS_LOG("the pack was made from another build of the game: make it again (make)\n");
		return -1;
	}

	const uint8_t *p = pages;
	uint32_t count = get32(&p);
	if (count * 4 + 4 > pages_size || count * CRS_PACK_PAGE > data_size) return -1;
	crs_memory_map(p, data, count);
	crs_memory_sink(0xC00000, 0xE00000);

	uint32_t regs[CRS_CPU_REGS];
	p = cpu;
	for (int i = 0; i < CRS_CPU_REGS; i++) regs[i] = get32(&p);
	cpu_load(regs);
	state_load(port);
	return 0;
}

/* Save states: "CRSS", version, CRS_IMAGE_ID, the registers, the port
   layer's state (size, then crs_state_save's bytes), the sound playing,
   then the memory (crs_memory_save). Little-endian. */
#define STATE_VERSION 2

int crs_save_state(FILE *f)
{
	uint32_t regs[CRS_CPU_REGS];
	crs_cpu_save(regs);
	uint32_t port_size = crs_state_save(NULL);
	uint32_t header[4] = { 0, STATE_VERSION, CRS_IMAGE_ID, port_size };
	uint8_t *buffer = malloc(16 + CRS_CPU_REGS * 4 + port_size + CRS_AUDIO_PLAY_SIZE);
	if (!buffer) return -1;

	uint8_t *p = buffer;
	memcpy(p, "CRSS", 4);
	p += 4;
	for (int i = 1; i < 4; i++) p = put32(p, header[i]);
	for (int i = 0; i < CRS_CPU_REGS; i++) p = put32(p, regs[i]);
	p += crs_state_save(p);
	crs_audio_play_save(p);
	p += CRS_AUDIO_PLAY_SIZE;

	int ok = fwrite(buffer, 1, p - buffer, f) == (size_t)(p - buffer) && !crs_memory_save(f);
	free(buffer);
	return ok ? 0 : -1;
}

int crs_load_state(FILE *f)
{
	uint8_t header[16];
	if (fread(header, 1, 16, f) != 16 || memcmp(header, "CRSS", 4)) return -1;
	const uint8_t *p = header + 4;
	uint32_t version = get32(&p), id = get32(&p), port_size = get32(&p);
	if (version != STATE_VERSION || id != CRS_IMAGE_ID || port_size > 65536) {
		CRS_LOG("save state of another version or build of the game\n");
		return -1;
	}

	uint32_t size = CRS_CPU_REGS * 4 + port_size + CRS_AUDIO_PLAY_SIZE;
	uint8_t *buffer = malloc(size);
	if (!buffer) return -1;
	if (fread(buffer, 1, size, f) != size || crs_memory_load(f)) {
		free(buffer);
		return -1;
	}

	for (int i = 0; i < NUMBER_OF_FILE_HANDLES; i++) {
		if (files[i]) fclose(files[i]);
		files[i] = NULL;
	}

	uint32_t regs[CRS_CPU_REGS];
	p = buffer;
	for (int i = 0; i < CRS_CPU_REGS; i++) regs[i] = get32(&p);
	cpu_load(regs);
	state_load(p);
	crs_audio_play_load(p + port_size);
	free(buffer);
	frame_done = 0;
	exited = 0;
	return 0;
}

int crs_init(const crs_config_t *c)
{
	config = *c;

	if (config.ram) {
		crs_memory_init(config.ram, config.ram_size);
	} else {
		/* Every page, and the tables. */
		uint32_t size = CRS_MEM_SIZE + 256 * 256 * sizeof(uintptr_t) + 4096;
		uint8_t *ram = malloc(size);
		if (!ram) return -1;
		crs_memory_init(ram, size);
	}

	char music_dir[1024];
	if (config.music_dir) snprintf(music_dir, sizeof music_dir, "%s", config.music_dir);
	else snprintf(music_dir, sizeof music_dir, "%s/MUSIC_DAT", config.data_dir);
	crs_audio_init(music_dir, config.music);

	m68k_init();
	m68k_set_cpu_type(M68K_CPU_TYPE_68020);
	m68k_set_instr_hook_callback(instruction_hook);
	m68k_pulse_reset();
	crs_bg_reset();

	if (config.pack) return load_pack();

#ifndef CRS_GNW
	char v110_dir[1024];
	snprintf(v110_dir, sizeof v110_dir, "%s/V110", config.data_dir);
	crs_bg_load(v110_dir);
#endif

	/* The image, its non-zero pages only. */
	FILE *f = fopen(config.image, "rb");
	if (!f) { CRS_LOG("%s: cannot open\n", config.image); return -1; }
	uint8_t page[CRS_PACK_PAGE];
	size_t n;
	for (uint32_t a = CRS_IMAGE_BASE; (n = fread(page, 1, sizeof page, f)) > 0; a += n) {
		for (size_t i = 0; i < n; i++) if (page[i]) { crs_write_bytes(a, page, n); break; }
	}
	fclose(f);

	/* The VBL and raster vectors start at an RTE, as in the ports. */
	crs_wr16(DUMMY_RTE, 0x4E73);
	crs_wr32(0x118, DUMMY_RTE);
	crs_wr32(0x138, DUMMY_RTE);

	/* As start_emulator: a0-a3 at the cleared process area, a4 = entry. */
	m68k_set_reg(M68K_REG_SR, 0x2000);
	m68k_set_reg(M68K_REG_A7, CRS_SYM_NEW_STACK);
	m68k_set_reg(M68K_REG_A0, CRS_HEAP_START);
	m68k_set_reg(M68K_REG_A1, CRS_HEAP_START);
	m68k_set_reg(M68K_REG_A2, CRS_HEAP_START);
	m68k_set_reg(M68K_REG_A3, CRS_HEAP_START);
	m68k_set_reg(M68K_REG_A4, CRS_SYM__start);
	m68k_set_reg(M68K_REG_PC, CRS_SYM__start);
	return 0;
}
