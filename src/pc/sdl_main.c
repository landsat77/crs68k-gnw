/*
 * PC front end (SDL2) of the portable port layer: plays the game in a
 * window at the X68000's 55.46 Hz, with sound, keyboard and game
 * controller. Shows the middle 240 lines on a 320 x 240 screen, as the
 * Game & Watch will.
 *
 * usage: crs_sdl [options]
 *   --pack FILE       start from a .crs pack (tools/make_crs.c), with its
 *                     music (songs missing from it: NAME/MUSIC/*.crm in the
 *                     pack's directory, as on the SD card)
 *   --image FILE      else the linked game (default build/game/sz2_host.bin)
 *   --data DIR        and its data (default game/data)
 *   --save DIR        high scores and settings (default build/save)
 *   --scale N         window scale (default 3)
 *   --full            all 256 lines instead of the middle 240
 *   --bg100           the game's own backgrounds (ver1.0x) instead of ver1.10's
 *   --no-music        no music streams
 *   --ram KB          the RAM for written pages, as on the Game & Watch
 *                     (default: enough for all 16 MB)
 *   --frames N        headless: run N ticks (frames, and polls while the game
 *                     waits without ending frames, as in the pause) as fast
 *                     as possible and exit
 *   --checkpoints F   write a checkpoint every 60 frames to F
 *   --shots DIR       headless: save the screen every 500 frames as BMP
 *   --shot-every N    headless: every N frames instead
 *   --input FILE      headless: the buttons, one line per change:
 *                     "tick joystick start" (joystick: CRS_JOY_* bits in
 *                     hex, held until the next line); # starts a comment
 *   --ram-log         headless: the frame each time 16 KB more RAM pages
 *                     are in use
 *   --load-state F    start from a save state (made with F5 or --save-state-at)
 *   --save-state-at N F  headless: save the state to F at frame N
 *   --wav F           headless: the sound to F (WAV, mono, 48 kHz), a VBL's
 *                     worth each tick
 *
 * Keys: arrows = directions, Z / Ctrl / Space = trigger A (shot),
 * X / Shift = trigger B, Return / P = START (pause; the X68000's ESC),
 * F1 = auto fire on/off, F2 = backgrounds ver1.10 / ver1.0x, F5 = save state, F9 = load it (save_dir/state.crss),
 * Escape = quit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL.h>
#include "crs.h"

#define VBL_SECONDS (1.0 / 55.46)
#define AUDIO_RATE 48000

static SDL_AudioDeviceID audio_device;

static void audio_callback(void *user, Uint8 *stream, int bytes)
{
	(void)user;
	crs_audio_mix((int16_t *)stream, bytes / 2, AUDIO_RATE);
}

static void audio_lock(int lock)
{
	if (lock) SDL_LockAudioDevice(audio_device);
	else SDL_UnlockAudioDevice(audio_device);
}

static void write_wav_header(FILE *f, uint32_t samples)
{
	uint32_t data = samples * 2;
	uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x01\0........\x02\0\x10\0data....";
	const uint32_t fields[4][2] = { { 4, 36 + data }, { 24, AUDIO_RATE }, { 28, AUDIO_RATE * 2 }, { 40, data } };
	for (int n = 0; n < 4; n++)
		for (int i = 0; i < 4; i++) h[fields[n][0] + i] = fields[n][1] >> (8 * i);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, sizeof h, f);
}

static void save_screen(const char *dir, int frame)
{
	static uint16_t screen[CRS_SCREEN_WIDTH * CRS_SCREEN_HEIGHT];
	crs_render(screen, CRS_SCREEN_WIDTH, 0, CRS_SCREEN_HEIGHT);
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(screen, CRS_SCREEN_WIDTH, CRS_SCREEN_HEIGHT, 16,
	                                                    CRS_SCREEN_WIDTH * 2, SDL_PIXELFORMAT_RGB565);
	char path[1024];
	snprintf(path, sizeof path, "%s/screen_%05d.bmp", dir, frame);
	SDL_SaveBMP(s, path);
	SDL_FreeSurface(s);
}

/* Directions, triggers and START from the keyboard and the first game
   controller. */
static void read_input(SDL_GameController *pad)
{
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	uint8_t j = 0;
	int start = 0;

	if (k[SDL_SCANCODE_UP]) j |= CRS_JOY_UP;
	if (k[SDL_SCANCODE_DOWN]) j |= CRS_JOY_DOWN;
	if (k[SDL_SCANCODE_LEFT]) j |= CRS_JOY_LEFT;
	if (k[SDL_SCANCODE_RIGHT]) j |= CRS_JOY_RIGHT;
	if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL] || k[SDL_SCANCODE_SPACE]) j |= CRS_JOY_A;
	if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) j |= CRS_JOY_B;
	if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_P]) start = 1;

	if (pad) {
		const int dead = 12000;
		if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) < -dead) j |= CRS_JOY_UP;
		if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) > dead) j |= CRS_JOY_DOWN;
		if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) < -dead) j |= CRS_JOY_LEFT;
		if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) > dead) j |= CRS_JOY_RIGHT;
		if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) j |= CRS_JOY_A;
		if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) j |= CRS_JOY_B;
		if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) start = 1;
	}

	/* Opposite directions cancel out (the game reads left + right as START). */
	if ((j & (CRS_JOY_LEFT | CRS_JOY_RIGHT)) == (CRS_JOY_LEFT | CRS_JOY_RIGHT)) j &= ~(CRS_JOY_LEFT | CRS_JOY_RIGHT);
	if ((j & (CRS_JOY_UP | CRS_JOY_DOWN)) == (CRS_JOY_UP | CRS_JOY_DOWN)) j &= ~(CRS_JOY_UP | CRS_JOY_DOWN);

	crs_input.joystick = j;
	crs_input.start = start;
}

int main(int argc, char **argv)
{
	crs_config_t config = {
		.image = "build/game/sz2_host.bin",
		.data_dir = "game/data",
		.music_dir = "game/music",
		.save_dir = "build/save",
		.music = 1,
	};
	int scale = 3, full = 0, frames = 0, shot_every = 500, ram_log = 0, save_at = -1;
	const char *checkpoints = NULL, *shots = NULL, *pack = NULL, *input = NULL;
	const char *load_state = NULL, *save_state = NULL, *wav = NULL;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--pack") && v) pack = v, i++;
		else if (!strcmp(a, "--image") && v) config.image = v, i++;
		else if (!strcmp(a, "--data") && v) config.data_dir = v, i++;
		else if (!strcmp(a, "--save") && v) config.save_dir = v, i++;
		else if (!strcmp(a, "--scale") && v) scale = atoi(v), i++;
		else if (!strcmp(a, "--frames") && v) frames = atoi(v), i++;
		else if (!strcmp(a, "--checkpoints") && v) checkpoints = v, i++;
		else if (!strcmp(a, "--shots") && v) shots = v, i++;
		else if (!strcmp(a, "--shot-every") && v) shot_every = atoi(v), i++;
		else if (!strcmp(a, "--input") && v) input = v, i++;
		else if (!strcmp(a, "--ram-log")) ram_log = 1;
		else if (!strcmp(a, "--load-state") && v) load_state = v, i++;
		else if (!strcmp(a, "--save-state-at") && v && i + 2 < argc) save_at = atoi(v), save_state = argv[i + 2], i += 2;
		else if (!strcmp(a, "--wav") && v) wav = v, i++;
		else if (!strcmp(a, "--full")) full = 1;
		else if (!strcmp(a, "--bg100")) crs_bg_enabled = 0;
		else if (!strcmp(a, "--no-music")) config.music = 0;
		else if (!strcmp(a, "--ram") && v) {
			config.ram_size = atoi(v) * 1024;
			config.ram = malloc(config.ram_size);
			i++;
		}
		else {
			fprintf(stderr, "unknown option %s (see the top of sdl_main.c)\n", a);
			return 2;
		}
	}

	static char music_dir[1024];
	if (pack) {
		FILE *f = fopen(pack, "rb");
		if (!f) { perror(pack); return 1; }
		fseek(f, 0, SEEK_END);
		config.pack_size = ftell(f);
		fseek(f, 0, SEEK_SET);
		uint8_t *data = malloc(config.pack_size);
		if (!data || fread(data, 1, config.pack_size, f) != config.pack_size) { perror(pack); return 1; }
		fclose(f);
		config.pack = data;
		config.data_dir = NULL;

		/* "dir/ChoRenSha.crs" -> "dir/ChoRenSha/MUSIC" */
		snprintf(music_dir, sizeof music_dir, "%s", pack);
		char *dot = strrchr(music_dir, '.');
		if (dot) *dot = 0;
		strncat(music_dir, "/MUSIC", sizeof music_dir - strlen(music_dir) - 1);
		config.music_dir = music_dir;
	}

	mkdir(config.save_dir, 0755);
	crs_input.autofire = 1;
	int headless = frames > 0;
	if (headless) config.quiet = 1;

	if (SDL_Init(headless ? 0 : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	if (crs_init(&config)) return 1;

	if (load_state) {
		FILE *f = fopen(load_state, "rb");
		if (!f || crs_load_state(f)) {
			fprintf(stderr, "%s: not a save state of this game\n", load_state);
			return 1;
		}
		fclose(f);
	}

	FILE *checkpoint_file = checkpoints ? fopen(checkpoints, "w") : NULL;
	if (checkpoint_file) fprintf(checkpoint_file, "# crs_sdl, lockstep\n# frame score random scroll sprites player-structure\n");

	if (headless) {
		/* The input script: (frame, joystick, start) changes. */
		static int script[4096][3];
		int script_lines = 0, next_line = 0;
		FILE *script_file = input ? fopen(input, "r") : NULL;
		if (input && !script_file) { perror(input); return 1; }
		char text[256];
		while (script_file && fgets(text, sizeof text, script_file) && script_lines < 4096) {
			int frame, start;
			unsigned joystick;
			if (text[0] != '#' && sscanf(text, "%d %x %d", &frame, &joystick, &start) == 3) {
				script[script_lines][0] = frame;
				script[script_lines][1] = joystick;
				script[script_lines][2] = start;
				script_lines++;
			}
		}
		if (script_file) fclose(script_file);

		/* Ticks: crs_run_frame calls, frames and polls (the pause loops
		   without ending frames); the script and --frames count them. */
		int last = 0, ticks = 0;
		uint32_t logged_pages = 0, wav_samples = 0;
		FILE *wav_file = wav ? fopen(wav, "wb") : NULL;
		if (wav && !wav_file) { perror(wav); return 1; }
		if (wav_file) write_wav_header(wav_file, 0);
		while (ticks < frames) {
			while (next_line < script_lines && script[next_line][0] <= ticks) {
				crs_input.joystick = script[next_line][1];
				crs_input.start = script[next_line][2];
				next_line++;
			}
			if (crs_run_frame() == CRS_EXITED) break;
			ticks++;
			if (wav_file) {
				static int16_t sound[2048];
				uint32_t total = (uint32_t)(ticks * VBL_SECONDS * AUDIO_RATE);
				crs_audio_mix(sound, total - wav_samples, AUDIO_RATE);
				fwrite(sound, 2, total - wav_samples, wav_file);
				wav_samples = total;
			}
			if (ram_log && crs_ram_pages >= logged_pages + 64) {
				logged_pages = crs_ram_pages;
				fprintf(stderr, "frame %d: %u RAM pages, %u tables\n", crs_frame_count, crs_ram_pages, crs_ram_tables);
			}
			if (crs_frame_count == last) continue;	/* a poll, no frame */
			last = crs_frame_count;
			if (checkpoint_file && crs_frame_count % 60 == 0) crs_write_checkpoint(checkpoint_file);
			if (shots && crs_frame_count % shot_every == 0) save_screen(shots, crs_frame_count);
			if (crs_frame_count == save_at) {
				FILE *f = fopen(save_state, "wb");
				if (!f || crs_save_state(f)) fprintf(stderr, "%s: cannot save the state\n", save_state);
				if (f) fclose(f);
			}
		}
		if (checkpoint_file) fclose(checkpoint_file);
		if (wav_file) {
			write_wav_header(wav_file, wav_samples);
			fclose(wav_file);
		}
		fprintf(stderr, "%d frames; RAM: %u pages now, %u at most, and %u tables: %u KB at most\n",
		        crs_frame_count, crs_ram_pages, crs_ram_pages_peak, crs_ram_tables,
		        crs_ram_pages_peak / 4 + crs_ram_tables);
		SDL_Quit();
		return 0;
	}

	int lines = full ? 256 : 240;
	SDL_Window *window = SDL_CreateWindow("Cho Ren Sha 68k", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                                      320 * scale, lines * scale, SDL_WINDOW_RESIZABLE);
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	SDL_RenderSetLogicalSize(renderer, 320, lines);
	SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
	                                         CRS_SCREEN_WIDTH, CRS_SCREEN_HEIGHT);

	SDL_AudioSpec want = { .freq = AUDIO_RATE, .format = AUDIO_S16SYS, .channels = 1, .samples = 1024,
	                       .callback = audio_callback };
	audio_device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	if (audio_device) {
		crs_audio_lock = audio_lock;
		SDL_PauseAudioDevice(audio_device, 0);
	} else {
		fprintf(stderr, "no sound: %s\n", SDL_GetError());
	}

	SDL_GameController *pad = NULL;
	static uint16_t screen[CRS_SCREEN_WIDTH * CRS_SCREEN_HEIGHT];
	double frequency = SDL_GetPerformanceFrequency();
	double next = SDL_GetPerformanceCounter() / frequency;
	int running = 1;

	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) running = 0;
			if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = 0;
			if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F2 && !e.key.repeat) {
				crs_bg_enabled = !crs_bg_enabled;
				fprintf(stderr, "backgrounds of ver%s\n", crs_bg_enabled ? "1.10" : "1.0x");
			}
			if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F1 && !e.key.repeat) {
				crs_input.autofire = !crs_input.autofire;
				fprintf(stderr, "auto fire %s\n", crs_input.autofire ? "on" : "off");
			}
			if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_F5 || e.key.keysym.sym == SDLK_F9) && !e.key.repeat) {
				char path[1100];
				snprintf(path, sizeof path, "%s/state.crss", config.save_dir);
				int save = e.key.keysym.sym == SDLK_F5;
				FILE *f = fopen(path, save ? "wb" : "rb");
				int ok = f && !(save ? crs_save_state(f) : crs_load_state(f));
				if (f) fclose(f);
				fprintf(stderr, "%s %s: %s\n", save ? "save" : "load", path, ok ? "done" : "failed");
			}
			if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
		}
		read_input(pad);

		int last = crs_frame_count;
		int vbls = crs_run_frame();
		if (vbls == CRS_EXITED) break;
		if (checkpoint_file && crs_frame_count != last && crs_frame_count % 60 == 0) crs_write_checkpoint(checkpoint_file);

		crs_render(screen, CRS_SCREEN_WIDTH, 0, CRS_SCREEN_HEIGHT);
		SDL_UpdateTexture(texture, NULL, screen, CRS_SCREEN_WIDTH * 2);
		SDL_RenderClear(renderer);
		SDL_Rect source = { 0, full ? 0 : 8, CRS_SCREEN_WIDTH, lines };
		SDL_Rect target = { 32, 0, CRS_SCREEN_WIDTH, lines };
		SDL_RenderCopy(renderer, texture, &source, &target);
		SDL_RenderPresent(renderer);

		/* The frame lasts its VBLs; too far behind (loading), start over. */
		next += vbls * VBL_SECONDS;
		double now = SDL_GetPerformanceCounter() / frequency;
		if (now - next > 0.25) next = now;
		while (next - now > 0.002) {
			SDL_Delay((Uint32)((next - now) * 1000) > 1 ? (Uint32)((next - now) * 1000) - 1 : 1);
			now = SDL_GetPerformanceCounter() / frequency;
		}
	}

	if (checkpoint_file) fclose(checkpoint_file);
	if (audio_device) {
		SDL_CloseAudioDevice(audio_device);
		crs_audio_lock = NULL;
	}
	SDL_Quit();
	return 0;
}
