/*
 * Cho Ren Sha 68k for Retro-Go SD (Game & Watch), on the portable port layer
 * (crs.h), built on retro-go-sd-templates. The game is a .crs pack made by
 * tools/make_crs.c (`make`):
 *
 * - as a homebrew (PROJECT_KIND_HOMEBREW, this repository's build):
 *   /homebrews/ChoRenSha.bin with ChoRenSha.crs beside it;
 * - as a core (PROJECT_KIND=core): the launcher would list the .crs packs
 *   in its system's folder and start app_main with one.
 *
 * - The pack is cached in external flash and read in place; the pages the
 *   game writes go to the RAM left in RAM_EMU (crs_memory.c).
 * - The picture: the middle 240 lines of the X68000's 256, centred in the
 *   320 x 240 LCD (RGB565).
 * - The game runs at 55.46 Hz: each loop is one VBL, the audio buffer
 *   (48 kHz, SAMPLES_PER_VBL) paces it; a game frame lasting several VBLs
 *   is shown once.
 * - Buttons: D-pad, A = shot (trigger A), B = trigger B, START (or GAME) =
 *   the X68000's ESC (pause; each press a clean pulse, see start_button).
 *   Pause menu: auto fire, music, backgrounds (ver1.10 or ver1.0x).
 * - Music: in the pack (ADPCM, cached in flash with the rest, so nothing
 *   is read from the SD card while playing). A song missing from it is
 *   streamed from <pack directory>/<pack name>/MUSIC/NAME.crm if there.
 * - High scores and settings: /retro-go/saves/crs68k.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "common.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "rom_manager.h"
#include "odroid_system.h"
#include "appid.h"
#include "odroid_overlay.h"
#include "odroid_settings.h"
#include "odroid_sdcard.h"
#include "gw_malloc.h"
#include "rg_storage.h"
#include "gw_core_bridge.h"

#include "crs.h"

#define SAMPLE_RATE 48000
#define SAMPLES_PER_VBL 866	/* 48000 / 55.46 Hz: 55.43 Hz */
#define FRAME_TIME_10US 1803	/* 1 / 55.46 Hz in 10 us */
#define RAM_MARGIN 8192		/* RAM_EMU left to the firmware's bump pool */
#define X_OFFSET ((WIDTH - CRS_SCREEN_WIDTH) / 2)
#define FIRST_LINE ((CRS_SCREEN_HEIGHT - HEIGHT) / 2)

#if defined(PROJECT_KIND_HOMEBREW)
#define APP_ID APPID_HOMEBREW
#else
#define APP_ID APPID_CORE
#endif

static char save_dir[] = RG_BASE_PATH_SAVES "/crs68k";
static uint32_t ram_pool_size;	/* for the error screen */
static char pack_path[256], music_dir[256];
static int vbls_left;		/* VBLs the current game frame still lasts */
static bool picture_ready;	/* a game frame ended since the last picture */
static int music_on = 1;
static char autofire_value[8], music_value[8], backgrounds_value[8];

/* --- picture --- */

static void blit(void)
{
	uint16_t *fb = lcd_get_active_buffer();
	for (int y = 0; y < HEIGHT; y++) {
		memset(fb + y * WIDTH, 0, X_OFFSET * 2);
		memset(fb + y * WIDTH + X_OFFSET + CRS_SCREEN_WIDTH, 0, (WIDTH - X_OFFSET - CRS_SCREEN_WIDTH) * 2);
	}
	crs_render(fb + X_OFFSET, WIDTH, FIRST_LINE, HEIGHT);
	common_ingame_overlay();
}

/* --- system callbacks --- */

/* The whole game (crs_save_state): registers, port layer, sound, and the
   pages that differ from the pack. */
static bool LoadState(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	bool ok = !crs_load_state(f);
	fclose(f);
	vbls_left = 0;
	picture_ready = true;
	return ok;
}

static bool SaveState(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	bool ok = !crs_save_state(f);
	fclose(f);
	return ok;
}

static void *Screenshot(void)
{
	lcd_wait_for_vblank();
	blit();
	return lcd_get_active_buffer();
}

static void Shutdown(void)
{
}

static void SleepWake(void)
{
	odroid_audio_init(SAMPLE_RATE);
	audio_clear_buffers();
	audio_start_playing(SAMPLES_PER_VBL);
}

static void SramSave(void)
{
}

/* --- pause menu --- */

static void option_value(char *value, int on)
{
	strcpy(value, on ? "ON" : "OFF");
}

static bool autofire_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
	(void)repeat;
	if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
		crs_input.autofire = !crs_input.autofire;
		odroid_settings_app_int32_set("autofire", crs_input.autofire);
	}
	option_value(option->value, crs_input.autofire);
	return event == ODROID_DIALOG_ENTER;
}

static bool music_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
	(void)repeat;
	if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
		music_on = !music_on;
		odroid_settings_app_int32_set("music", music_on);
		crs_audio_set_music(music_on);
	}
	option_value(option->value, music_on);
	return event == ODROID_DIALOG_ENTER;
}

static void backgrounds_value_set(void)
{
	strcpy(backgrounds_value, crs_bg_enabled ? "1.10" : "1.0x");
}

static bool backgrounds_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat)
{
	(void)repeat;
	if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
		crs_bg_enabled = !crs_bg_enabled;
		odroid_settings_app_int32_set("bg110", crs_bg_enabled);
	}
	backgrounds_value_set();
	(void)option;
	return event == ODROID_DIALOG_ENTER;
}

/* --- buttons --- */

/* START as the game's pause sees it. The pause waits for START released,
   pressed, released again (made for the X68000's ESC key): on the device a
   press was seen more than once, and the pause ended at once (40 ms of
   debouncing did not help). So the game only gets clean presses: a press
   of the button (its edge; how long it is held does not matter) becomes
   START for START_PULSE calls of crs_run_frame (the game's frames and
   polls, so it cannot miss it), and the button is not looked at again for
   START_REFRACTORY_MS. */
#define START_PULSE 3
#define START_REFRACTORY_MS 300

static int start_pulse;	/* crs_run_frame calls START still lasts */

static void start_button(int pressed)
{
	static int was_pressed;
	static uint32_t last_press;
	uint32_t now = HAL_GetTick();

	if (pressed && !was_pressed && now - last_press >= START_REFRACTORY_MS) {
		start_pulse = START_PULSE;
		last_press = now;
	}
	was_pressed = pressed;
}

/* --- errors --- */

/* The message, what the RAM held, and back to the launcher on a button
   (once all are released: the one that led here may still be held). */
static void halt(const char *message)
{
	char line[64];
	lcd_clear_buffers();
	odroid_overlay_draw_text(8, 80, WIDTH - 16, "Cho Ren Sha 68k:", 0xFFFF, 0x0000);
	odroid_overlay_draw_text(8, 96, WIDTH - 16, message, 0xFFFF, 0x0000);
	snprintf(line, sizeof line, "RAM %lu KB: %lu pages (%lu at most)",
	         (unsigned long)(ram_pool_size / 1024), (unsigned long)crs_ram_pages, (unsigned long)crs_ram_pages_peak);
	odroid_overlay_draw_text(8, 120, WIDTH - 16, line, 0xFFFF, 0x0000);
	snprintf(line, sizeof line, "%lu tables, frame %d", (unsigned long)crs_ram_tables, crs_frame_count);
	odroid_overlay_draw_text(8, 136, WIDTH - 16, line, 0xFFFF, 0x0000);
	odroid_overlay_draw_text(8, 160, WIDTH - 16, "Press a button: back to the menu", 0xFFFF, 0x0000);
	lcd_swap();

	odroid_gamepad_state_t joystick;
	bool released = false;
	while (1) {
		wdog_refresh();
		HAL_Delay(50);
		odroid_input_read_gamepad(&joystick);
		bool any = false;
		for (int i = 0; i < ODROID_INPUT_MAX; i++) any |= joystick.values[i] != 0;
		if (!any) released = true;
		else if (released) odroid_system_switch_app(0);
	}
}

static void fatal(const char *message)
{
	printf("crs68k: %s\n", message);
	halt(message);
}

/* --- main --- */

void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
	odroid_gamepad_state_t joystick;
	odroid_dialog_choice_t options[4];

	gw_core_bridge_init();

	if (start_paused) {
		common_emu_state.pause_after_frames = 2;
		odroid_audio_mute(true);
	} else {
		common_emu_state.pause_after_frames = 0;
	}
	common_emu_state.frame_time_10us = FRAME_TIME_10US;
	lcd_set_refresh_rate(60);

	odroid_system_init(APP_ID, SAMPLE_RATE);
	odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, &Shutdown, &SleepWake, &SramSave, NULL);

	crs_input.autofire = odroid_settings_app_int32_get("autofire", 1) ? 1 : 0;
	music_on = odroid_settings_app_int32_get("music", 1) ? 1 : 0;
	option_value(autofire_value, crs_input.autofire);
	option_value(music_value, music_on);
	crs_bg_enabled = odroid_settings_app_int32_get("bg110", 1) ? 1 : 0;
	backgrounds_value_set();
	options[0] = (odroid_dialog_choice_t){ .id = 100, .label = "Auto fire", .value = autofire_value,
	                                       .enabled = 1, .update_cb = &autofire_cb };
	options[1] = (odroid_dialog_choice_t){ .id = 101, .label = "Music", .value = music_value,
	                                       .enabled = 1, .update_cb = &music_cb };
	options[2] = (odroid_dialog_choice_t){ .id = 102, .label = "Backgrounds", .value = backgrounds_value,
	                                       .enabled = 1, .update_cb = &backgrounds_cb };
	options[3] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;

	lcd_clear_buffers();
	audio_start_playing(SAMPLES_PER_VBL);

	/* The pack, in flash: the ROM the launcher gives a core; beside a
	   homebrew's .bin ("/homebrews/ChoRenSha.bin" ->
	   "/homebrews/ChoRenSha.crs"). */
	if (!ACTIVE_FILE || !ACTIVE_FILE->path[0]) fatal("no pack");
	snprintf(pack_path, sizeof pack_path, "%s", ACTIVE_FILE->path);
#if defined(PROJECT_KIND_HOMEBREW)
	char *extension = strrchr(pack_path, '.');
	if (extension) *extension = 0;
	strncat(pack_path, ".crs", sizeof pack_path - strlen(pack_path) - 1);
#endif
	FILE *f = fopen(pack_path, "rb");
	if (!f) fatal("pack (.crs) not found");
	fseek(f, 0, SEEK_END);
	uint32_t pack_size = ftell(f);
	fclose(f);
	const uint8_t *pack = odroid_overlay_cache_file_in_flash(pack_path, &pack_size, false);
	if (!pack || !pack_size) fatal("cannot map the pack in flash");

	/* ".../ChoRenSha.crs" -> ".../ChoRenSha/MUSIC" */
	snprintf(music_dir, sizeof music_dir, "%s", pack_path);
	char *dot = strrchr(music_dir, '.');
	if (dot) *dot = 0;
	strncat(music_dir, "/MUSIC", sizeof music_dir - strlen(music_dir) - 1);

	odroid_sdcard_mkdir(save_dir);

	/* The written pages: the rest of RAM_EMU. */
	uint32_t ram_size = ram_get_free_size();
	if (ram_size < RAM_MARGIN + 64 * 1024) fatal("not enough RAM");
	ram_size -= RAM_MARGIN;
	ram_pool_size = ram_size;
	uint8_t *ram = ram_malloc(ram_size);
	if (!ram) fatal("cannot allocate RAM");
	printf("crs68k: pack %lu bytes at %p, %lu KB of RAM for pages\n", (unsigned long)pack_size, (void *)pack,
	       (unsigned long)(ram_size / 1024));

	crs_fatal = fatal;
	crs_config_t config = {
		.pack = pack,
		.pack_size = pack_size,
		.save_dir = save_dir,
		.music_dir = music_dir,
		.music = music_on,
		.quiet = 1,
		.ram = ram,
		.ram_size = ram_size,
		.tick = wdog_refresh,
	};
	if (crs_init(&config)) fatal("the pack does not match this core: make it again");

	if (load_state) odroid_system_emu_load_state(save_slot);

	while (1) {
		wdog_refresh();

		bool draw_frame = common_emu_frame_loop();

		odroid_input_read_gamepad(&joystick);
		common_emu_input_loop(&joystick, options, &blit);
		common_emu_input_loop_handle_turbo(&joystick);

		uint8_t j = 0;
		if (joystick.values[ODROID_INPUT_UP]) j |= CRS_JOY_UP;
		if (joystick.values[ODROID_INPUT_DOWN]) j |= CRS_JOY_DOWN;
		if (joystick.values[ODROID_INPUT_LEFT]) j |= CRS_JOY_LEFT;
		if (joystick.values[ODROID_INPUT_RIGHT]) j |= CRS_JOY_RIGHT;
		if (joystick.values[ODROID_INPUT_A]) j |= CRS_JOY_A;
		if (joystick.values[ODROID_INPUT_B]) j |= CRS_JOY_B;
		crs_input.joystick = j;
		start_button(joystick.values[ODROID_INPUT_X] || joystick.values[ODROID_INPUT_START]);

		/* One VBL: the next game frame when the current one is over (frames
		   of 0 VBLs follow at once, up to a few). */
		for (int guard = 0; vbls_left == 0 && guard < 8; guard++) {
			crs_input.start = start_pulse > 0;
			if (start_pulse > 0) start_pulse--;
			int n = crs_run_frame();
			if (n == CRS_EXITED) odroid_system_switch_app(0);
			vbls_left = n;
			picture_ready = true;
		}
		if (vbls_left > 0) vbls_left--;

		if (draw_frame && picture_ready) {
			blit();
			lcd_swap();
			picture_ready = false;
		}

		if (!common_emu_sound_loop_is_muted()) {
			int16_t *buffer = audio_get_active_buffer();
			uint16_t length = audio_get_buffer_length();
			if (buffer && length) {
				crs_audio_mix(buffer, length, SAMPLE_RATE);
				int volume = common_emu_sound_get_volume();
				for (uint16_t i = 0; i < length; i++) buffer[i] = buffer[i] * volume / 255;
			}
		}
		common_emu_sound_sync(false);
	}
}
