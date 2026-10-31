// guasa-game: the game window (PLAN.md section 3). The Electron interface will
// open it for each match and talk to it over stdio; for now it also runs alone:
//
//   guasa-game check <roms>      checks the ROM folder, one line per problem
//   guasa-game versus <roms>     two players on this PC
//   guasa-game test <roms>       Test Game: versus plus both players' buttons on screen,
//                                as pressed and as the game reads them (SOCD rule)
//   guasa-game online <roms> <1|2> <local_port> (<ip:port> | relay=<host:port> token=<hex> [relayonly]) [delay=N|auto] [lag=MS] [jitter=MS] [loss=PCT]
//                                online match (rollback, net.cpp); the keyboard and the
//                                first control play this side
//
// In the window: F1 sets up a control, F11 or Alt+Enter = full screen.
// Online also: F2 shows the buttons, F5 / F6 = less / more delay, Esc leaves.
// Online, for the interface: "resultado 1|2" on stdout when a game ends, and
// "salir" on stdin closes the window (the set is over).
// Every match starts from the starting state generated here (guasa_boot_to_select),
// and the window keeps the game's own pace, 59.63 frames per second.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <SDL3/SDL.h>
#include "guasa_core.h"
#include "net.h"
#include "results.h"
#include <atomic>
#include <thread>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

// ---------------------------------------------------------------------------
// Controls. A device (the keyboard or a joystick) has one bind per action.

enum { kActions = 12 };
static const char* kActionNames[kActions] = { "ARRIBA", "ABAJO", "IZQUIERDA", "DERECHA", "PUNO DEBIL",
	"PUNO MEDIO", "PUNO FUERTE", "PATADA DEBIL", "PATADA MEDIA", "PATADA FUERTE", "START", "MONEDA" };
static const char* kShort[kActions] = { "Ar", "Ab", "Iz", "De", "LP", "MP", "HP", "LK", "MK", "HK", "St", "Mo" };

struct Bind { char kind; int index, value, rest; };   // 'k' key, 'b' button, 'h' hat, 'a' axis, 0 none
struct Mapping { std::string name; Bind b[kActions]; };

static std::map<std::string, Mapping> g_maps;   // by device id: "teclado" or the joystick's GUID

static std::string MapsPath()
{
	char* dir = SDL_GetPrefPath("La Guasabara", "juego");
	std::string p = std::string(dir ? dir : "") + "controles.txt";
	SDL_free(dir);
	return p;
}

static void LoadMaps()
{
	FILE* f = fopen(MapsPath().c_str(), "r");
	if (!f) return;
	char line[512], id[128], kind;
	int a, index, value, rest;
	std::string name;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#') {   // "# <id> <name>" starts a device
			if (sscanf(line, "# %127s", id) == 1) {
				const char* n = strchr(line + 2, ' ');
				name = n ? n + 1 : "";
				while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
				g_maps[id] = Mapping{ name, {} };
			}
			continue;
		}
		if (sscanf(line, "%d %c %d %d %d", &a, &kind, &index, &value, &rest) == 5 && a >= 0 && a < kActions &&
		    kind != '-' && !g_maps.empty())
			g_maps[id].b[a] = { kind, index, value, rest };
	}
	fclose(f);
}

static void SaveMaps()
{
	FILE* f = fopen(MapsPath().c_str(), "w");
	if (!f) return;
	for (auto& m : g_maps) {
		fprintf(f, "# %s %s\n", m.first.c_str(), m.second.name.c_str());
		for (int i = 0; i < kActions; i++) {
			const Bind& b = m.second.b[i];
			fprintf(f, "%d %c %d %d %d\n", i, b.kind ? b.kind : '-', b.index, b.value, b.rest);
		}
	}
	fclose(f);
}

// Keyboard without a setup: arrows; A S D = punches; Z X C = kicks; Enter = start; 5 = coin.
static Mapping KeyboardDefault()
{
	static const SDL_Scancode k[kActions] = { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
		SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C,
		SDL_SCANCODE_RETURN, SDL_SCANCODE_5 };
	Mapping m{ "Teclado", {} };
	for (int i = 0; i < kActions; i++) m.b[i] = { 'k', (int)k[i], 1, 0 };
	return m;
}

struct Device {
	SDL_Joystick* joy;
	SDL_Gamepad* pad;           // when SDL knows it as a gamepad (used only without a setup)
	std::string id, name;
	std::vector<int> rest;      // each axis at rest, read when it was connected
};
static std::vector<Device> g_devs;   // joysticks in the order they were connected

static std::string GuidOf(SDL_JoystickID which)
{
	char s[64];
	SDL_GUIDToString(SDL_GetJoystickGUIDForID(which), s, sizeof(s));
	return s;
}

// Some controls (the 8BitDo Ultimate 2 for one) also show up as extra devices
// with no hats or axes; those are skipped unless SDL knows them as a gamepad.
static void AddDevice(SDL_JoystickID which)
{
	for (auto& d : g_devs) if (SDL_GetJoystickID(d.joy) == which) return;
	SDL_Joystick* j = SDL_OpenJoystick(which);
	if (!j) return;
	if (!SDL_IsGamepad(which) && SDL_GetNumJoystickHats(j) == 0 && SDL_GetNumJoystickAxes(j) == 0) {
		SDL_CloseJoystick(j);
		return;
	}
	Device d{ j, SDL_IsGamepad(which) ? SDL_OpenGamepad(which) : NULL, GuidOf(which), SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "Control" };
	for (int a = 0; a < SDL_GetNumJoystickAxes(j); a++) d.rest.push_back(SDL_GetJoystickAxis(j, a));
	g_devs.push_back(d);
	printf("control conectado: %s\n", d.name.c_str());
	fflush(stdout);
}

static void RemoveDevice(SDL_JoystickID which)
{
	for (size_t i = 0; i < g_devs.size(); i++)
		if (SDL_GetJoystickID(g_devs[i].joy) == which) {
			printf("control desconectado: %s\n", g_devs[i].name.c_str());
			fflush(stdout);
			if (g_devs[i].pad) SDL_CloseGamepad(g_devs[i].pad);
			SDL_CloseJoystick(g_devs[i].joy);
			g_devs.erase(g_devs.begin() + i);
			return;
		}
}

static bool BindDown(const Device* d, const Bind& b)
{
	switch (b.kind) {
	case 'k': return SDL_GetKeyboardState(NULL)[b.index];
	case 'b': return d && SDL_GetJoystickButton(d->joy, b.index);
	case 'h': return d && (SDL_GetJoystickHat(d->joy, b.index) & b.value) != 0;
	case 'a': return d && (SDL_GetJoystickAxis(d->joy, b.index) - b.rest) * b.value > 16000;
	default: return false;
	}
}

// Gamepad without a setup: d-pad or left stick; X Y RB = punches; A B RT = kicks; Start; Back = coin.
static uint16_t GamepadDefault(SDL_Gamepad* g)
{
	static const SDL_GamepadButton buttons[kActions] = { SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
		SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
		SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_INVALID,
		SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK };
	uint16_t in = 0;
	for (int i = 0; i < kActions; i++)
		if (buttons[i] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(g, buttons[i])) in |= 1 << i;
	if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) in |= GUASA_HK;
	int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
	if (y < -16000) in |= GUASA_UP;
	if (y > 16000) in |= GUASA_DOWN;
	if (x < -16000) in |= GUASA_LEFT;
	if (x > 16000) in |= GUASA_RIGHT;
	return in;
}

static uint16_t ReadDevice(const Device& d)
{
	auto m = g_maps.find(d.id);
	if (m == g_maps.end()) return d.pad ? GamepadDefault(d.pad) : 0;
	uint16_t in = 0;
	for (int i = 0; i < kActions; i++) if (BindDown(&d, m->second.b[i])) in |= 1 << i;
	return in;
}

// Player 1: the keyboard and the first control; player 2: the second control.
static void ReadPlayers(uint16_t in[2])
{
	auto k = g_maps.find("teclado");
	const Mapping& keys = k != g_maps.end() ? k->second : KeyboardDefault();
	in[0] = in[1] = 0;
	for (int i = 0; i < kActions; i++) if (BindDown(NULL, keys.b[i])) in[0] |= 1 << i;
	for (size_t p = 0; p < g_devs.size() && p < 2; p++) in[p] |= ReadDevice(g_devs[p]);
}

// ---------------------------------------------------------------------------
// Window, picture and sound

static SDL_Window* g_win;
static SDL_Renderer* g_ren;
static SDL_Texture* g_tex;
static SDL_AudioStream* g_aud;
static int g_w, g_h;

static bool OpenWindow(const char* title)
{
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return false; }
	guasa_framebuffer(&g_w, &g_h, NULL);
	g_win = SDL_CreateWindow(title, g_h * 3 * 4 / 3, g_h * 3, SDL_WINDOW_RESIZABLE);   // 4:3 like the arcade monitor
	g_ren = g_win ? SDL_CreateRenderer(g_win, NULL) : NULL;
	if (!g_ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return false; }
	SDL_SetRenderVSync(g_ren, 0);   // the pace comes from the game's clock, not the monitor
	g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, g_w, g_h);
	SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_NEAREST);
	SDL_AudioSpec spec{ SDL_AUDIO_S16, 2, GUASA_SOUND_RATE };
	g_aud = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (g_aud) SDL_ResumeAudioStreamDevice(g_aud);
	return true;
}

static void PushAudio()
{
	int n = 0;
	const int16_t* s = guasa_audio(&n);
	// keep at most ~150 ms queued so the sound doesn't fall behind the picture
	if (g_aud && SDL_GetAudioStreamQueued(g_aud) < GUASA_SOUND_RATE * 4 * 15 / 100)
		SDL_PutAudioStreamData(g_aud, s, n * 4);
}

// The game, 4:3 and centered, with black bars on the sides that are left over.
static SDL_FRect GameRect()
{
	int ww, wh;
	SDL_GetRenderOutputSize(g_ren, &ww, &wh);
	float w = (float)ww, h = (float)wh;
	if (w * 3 > h * 4) w = h * 4 / 3; else h = w * 3 / 4;
	return { (ww - w) / 2, (wh - h) / 2, w, h };
}

// Text with SDL's built-in 8x8 font (no accents), scaled.
static void Text(float x, float y, float scale, const char* s, Uint8 r = 255, Uint8 g = 255, Uint8 b = 255)
{
	SDL_SetRenderScale(g_ren, scale, scale);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
	SDL_RenderDebugText(g_ren, x / scale + 1, y / scale + 1, s);   // shadow
	SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
	SDL_RenderDebugText(g_ren, x / scale, y / scale, s);
	SDL_SetRenderScale(g_ren, 1, 1);
}

// Test Game: one row per player with what the control sends and one with what
// the game reads after the opposite-directions rule.
static void DrawInputs(const SDL_FRect& r, const uint16_t raw[2])
{
	float s = r.h / 450 > 1 ? r.h / 450 : 1, cell = 22 * s;
	for (int p = 0; p < 2; p++) {
		uint16_t rows[2] = { raw[p], guasa_socd(raw[p]) };
		float x0 = p == 0 ? r.x + 8 * s : r.x + r.w - (kActions * cell + 8 * s);
		for (int row = 0; row < 2; row++) {
			float y = r.y + r.h - (2 - row) * (cell + 4 * s) - 40 * s;
			Text(x0, y - 11 * s, s, row == 0 ? (p == 0 ? "P1 control" : "P2 control") : (p == 0 ? "P1 juego" : "P2 juego"));
			for (int i = 0; i < kActions; i++) {
				bool on = rows[row] & (1 << i);
				bool cleaned = row == 1 && (raw[p] & (1 << i)) && !on;   // the rule took it out
				SDL_FRect c{ x0 + i * cell, y, cell - 2 * s, cell - 2 * s };
				SDL_SetRenderDrawColor(g_ren, on ? 230 : cleaned ? 160 : 30, on ? 180 : cleaned ? 40 : 30, on ? 40 : 30, 220);
				SDL_RenderFillRect(g_ren, &c);
				Text(c.x + 3 * s, c.y + 7 * s, s, kShort[i], on ? 0 : 200, on ? 0 : 200, on ? 0 : 200);
			}
		}
	}
	Text(r.x + 8 * s, r.y + r.h - 16 * s, s, "Izq+Der = neutral   Arriba+Abajo = arriba   (rojo: lo quito la regla)", 200, 200, 200);
}

static void DrawStats(const NetStats& st, int local);

static void Present(const uint16_t* raw, const char* banner, const NetStats* stats = NULL, int local = 0)
{
	SDL_UpdateTexture(g_tex, NULL, guasa_framebuffer(NULL, NULL, NULL), g_w * 2);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
	SDL_RenderClear(g_ren);
	SDL_FRect r = GameRect();
	SDL_RenderTexture(g_ren, g_tex, NULL, &r);
	if (raw) DrawInputs(r, raw);
	if (stats) DrawStats(*stats, local);
	if (banner) {
		float s = r.h / 300 > 1 ? r.h / 300 : 1;
		SDL_FRect box{ r.x, r.y + r.h / 2 - 40 * s, r.w, 80 * s };
		SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 200);
		SDL_RenderFillRect(g_ren, &box);
		const char* nl = strchr(banner, '\n');
		std::string first = nl ? std::string(banner, nl) : banner;
		Text(r.x + 12 * s, box.y + 16 * s, s * 1.5f, first.c_str(), 255, 220, 120);
		if (nl) Text(r.x + 12 * s, box.y + 50 * s, s, nl + 1);
	}
	SDL_RenderPresent(g_ren);
}

// ---------------------------------------------------------------------------
// Setting up a control inside the window (F1). The first device that sends
// anything is the one being set up; each action waits until the player
// presses something, and Esc leaves it without a bind.

struct Capture { bool active; int action; std::string id, name; Mapping m; std::vector<int> rest; int heldAxis; };
static Capture g_cap;

static void StartCapture()
{
	g_cap = Capture{ true, -1, "", "", {}, {}, -1 };
}

static bool Captured(const Bind& b)
{
	g_cap.m.b[g_cap.action] = b;
	g_cap.action++;
	if (g_cap.action == kActions) {
		g_cap.m.name = g_cap.name;
		g_maps[g_cap.id] = g_cap.m;
		SaveMaps();
		printf("control configurado: %s\n", g_cap.name.c_str());
		fflush(stdout);
		g_cap.active = false;
	}
	return true;
}

// Returns true if the event was used by the setup.
static bool CaptureEvent(const SDL_Event& ev)
{
	if (!g_cap.active) return false;
	if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE) {
		if (g_cap.action < 0) { g_cap.active = false; return true; }   // cancel before starting
		return Captured(Bind{ 0, 0, 0, 0 });
	}
	// which device: the first that sends something
	if (g_cap.action < 0) {
		if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) { g_cap.id = "teclado"; g_cap.name = "Teclado"; }
		else if (ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || ev.type == SDL_EVENT_JOYSTICK_HAT_MOTION) {
			for (auto& d : g_devs)
				if (SDL_GetJoystickID(d.joy) == (ev.type == SDL_EVENT_JOYSTICK_HAT_MOTION ? ev.jhat.which : ev.jbutton.which)) {
					g_cap.id = d.id; g_cap.name = d.name; g_cap.rest = d.rest;
				}
		}
		if (!g_cap.id.empty()) g_cap.action = 0;   // that press only picks the device
		return ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN;
	}
	bool keyboard = g_cap.id == "teclado";
	auto mine = [&](SDL_JoystickID which) {
		for (auto& d : g_devs) if (SDL_GetJoystickID(d.joy) == which) return d.id == g_cap.id;
		return false;
	};
	int a = g_cap.action;
	if (keyboard) {
		if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) return Captured(Bind{ 'k', (int)ev.key.scancode, 1, 0 });
		return false;
	}
	if (ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && mine(ev.jbutton.which))
		return Captured(Bind{ 'b', ev.jbutton.button, 1, 0 });
	if (ev.type == SDL_EVENT_JOYSTICK_HAT_MOTION && mine(ev.jhat.which) && ev.jhat.value) {
		// a single direction even if a diagonal shows up first
		Uint8 want = a == 0 ? SDL_HAT_UP : a == 1 ? SDL_HAT_DOWN : a == 2 ? SDL_HAT_LEFT : a == 3 ? SDL_HAT_RIGHT : 0;
		if (want && !(ev.jhat.value & want)) return false;
		return Captured(Bind{ 'h', ev.jhat.hat, want ? want : ev.jhat.value, 0 });
	}
	if (ev.type == SDL_EVENT_JOYSTICK_AXIS_MOTION && mine(ev.jaxis.which)) {
		int rest = ev.jaxis.axis < (int)g_cap.rest.size() ? g_cap.rest[ev.jaxis.axis] : 0;
		int d = ev.jaxis.value - rest;
		if (ev.jaxis.axis == g_cap.heldAxis) {   // the stick that set the last action: wait until it's back
			if (abs(d) < 8000) g_cap.heldAxis = -1;
			return false;
		}
		if (abs(d) > 16000) {
			g_cap.heldAxis = ev.jaxis.axis;
			return Captured(Bind{ 'a', ev.jaxis.axis, d > 0 ? 1 : -1, rest });
		}
	}
	return false;
}

static std::string CaptureBanner()
{
	if (g_cap.action < 0) return "CONFIGURAR CONTROL\nPresiona cualquier boton en el control que quieres configurar (Esc = cancelar)";
	return std::string("Presiona: ") + kActionNames[g_cap.action] + "\n" + g_cap.name + "   (" +
	       std::to_string(g_cap.action + 1) + " de 12, Esc = dejarlo sin asignar)";
}

// ---------------------------------------------------------------------------

static void CloseAll();

static int Play(const char* roms, bool test)
{
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) {
		char report[4000] = "";
		guasa_check_rom(roms, report, sizeof(report));
		printf("error: %s\n%s", msg, report);
		return 1;
	}
	guasa_boot_to_select();
	printf("estado inicial %016llx\n", (unsigned long long)guasa_state_hash());
	fflush(stdout);

	if (!OpenWindow(test ? "La Guasabara - Test Game" : "La Guasabara - Versus local")) return 1;
	LoadMaps();

	const Uint64 frameNs = (Uint64)(1e9 / guasa_fps());   // 59.63 frames per second
	Uint64 next = SDL_GetTicksNS();
	bool quit = false;
	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (CaptureEvent(ev)) continue;
			switch (ev.type) {
			case SDL_EVENT_QUIT: quit = true; break;
			case SDL_EVENT_JOYSTICK_ADDED: AddDevice(ev.jdevice.which); break;
			case SDL_EVENT_JOYSTICK_REMOVED: RemoveDevice(ev.jdevice.which); break;
			case SDL_EVENT_KEY_DOWN:
				if (ev.key.scancode == SDL_SCANCODE_F1) StartCapture();
				if (ev.key.scancode == SDL_SCANCODE_F11 || (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)))
					SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
				break;
			}
		}
		uint16_t in[2] = { 0, 0 };
		if (!g_cap.active) ReadPlayers(in);   // while setting up a control the game gets nothing
		guasa_run_frame(in, 1);
		PushAudio();
		std::string banner = g_cap.active ? CaptureBanner() : "";
		Present(test ? in : NULL, banner.empty() ? NULL : banner.c_str());

		next += frameNs;
		Uint64 now = SDL_GetTicksNS();
		if (next > now) SDL_DelayPrecise(next - now);
		else if (now - next > 5 * frameNs) next = now;   // far behind (window dragged, PC busy): don't rush to catch up
	}
	CloseAll();
	return 0;
}

static void CloseAll()
{
	for (auto& d : g_devs) { if (d.pad) SDL_CloseGamepad(d.pad); SDL_CloseJoystick(d.joy); }
	g_devs.clear();
	if (g_aud) SDL_DestroyAudioStream(g_aud);
	SDL_Quit();
	guasa_exit();
}

// Top line in online matches, like Fightcade's: ping, jitter, delay and rollback.
static void DrawStats(const NetStats& st, int local)
{
	SDL_FRect r = GameRect();
	float s = r.h / 450 > 1 ? r.h / 450 : 1;
	char line[200];
	snprintf(line, sizeof(line), "P%d   ping %u ms   jitter %.0f ms   delay %d   rollback %d (max %d)",
	         local + 1, st.ping, st.jitter, st.delay, st.lastRollback, st.maxRollback);
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 150);
	SDL_FRect box{ r.x, r.y, r.w, 14 * s };
	SDL_RenderFillRect(g_ren, &box);
	Text(r.x + 6 * s, r.y + 3 * s, s, line, 220, 220, 220);
	if (st.desyncs) Text(r.x + r.w - 150 * s, r.y + 3 * s, s, "DESINCRONIZADO", 255, 80, 80);
}

static int PlayOnline(const char* roms, const NetConfig& cfg)
{
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) {
		char report[4000] = "";
		guasa_check_rom(roms, report, sizeof(report));
		printf("error: %s\n%s", msg, report);
		return 1;
	}
	guasa_boot_to_select();
	printf("estado inicial %016llx\n", (unsigned long long)guasa_state_hash());
	printf("conectando\n");
	fflush(stdout);

	// The route to the other player is found before the window opens (a few
	// seconds at most), so the window never looks frozen.
	NetMatch net;
	std::string err;
	if (!net.Start(cfg, &err)) { printf("error: %s\n", err.c_str()); guasa_exit(); return 1; }
	printf("ruta %s\n", net.RouteText().c_str());
	fflush(stdout);
	if (!OpenWindow("La Guasabara - En linea")) { net.Stop(); return 1; }
	LoadMaps();

	// The interface can close the window by writing "salir" (the set is over).
	static std::atomic<bool> leave{ false };
	std::thread([] {
		std::string line;
		while (std::getline(std::cin, line))
			if (line.rfind("salir", 0) == 0) { leave = true; return; }
	}).detach();

	ResultWatcher results;
	const Uint64 frameNs = (Uint64)(1e9 / guasa_fps());   // 59.63 frames per second
	Uint64 next = SDL_GetTicksNS(), endedAt = 0;
	bool quit = false, showInputs = false, wasPlaying = false;
	int shownDelay = -1;
	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (CaptureEvent(ev)) continue;
			switch (ev.type) {
			case SDL_EVENT_QUIT: quit = true; break;
			case SDL_EVENT_JOYSTICK_ADDED: AddDevice(ev.jdevice.which); break;
			case SDL_EVENT_JOYSTICK_REMOVED: RemoveDevice(ev.jdevice.which); break;
			case SDL_EVENT_KEY_DOWN:
				switch (ev.key.scancode) {
				case SDL_SCANCODE_ESCAPE: quit = true; break;
				case SDL_SCANCODE_F1: StartCapture(); break;
				case SDL_SCANCODE_F2: showInputs = !showInputs; break;
				case SDL_SCANCODE_F5: net.SetDelay(net.Stats().delay - 1); break;
				case SDL_SCANCODE_F6: net.SetDelay(net.Stats().delay + 1); break;
				default: break;
				}
				if (ev.key.scancode == SDL_SCANCODE_F11 || (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)))
					SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
				break;
			}
		}

		uint16_t in[2] = { 0, 0 };
		if (!g_cap.active) ReadPlayers(in);   // this side plays with player 1's controls
		bool shown = false;
		NetMatch::Status st = net.Step(in[0], true, &shown);
		const NetStats& stats = net.Stats();
		if (shown) {
			PushAudio();
			if (int w = results.Update()) { printf("resultado %d\n", w); fflush(stdout); }
		}
		if (leave) quit = true;
		if (st == NetMatch::Playing && !wasPlaying) { printf("empezo la partida\n"); fflush(stdout); wasPlaying = true; }
		if (stats.delay != shownDelay) { printf("delay %d\n", stats.delay); fflush(stdout); shownDelay = stats.delay; }

		std::string banner;
		if (g_cap.active) banner = CaptureBanner();
		else if (st == NetMatch::Waiting) banner = "CONECTANDO\n" + net.RouteText() + "   (Esc = salir)";
		else if (st == NetMatch::Ended) {
			if (!endedAt) { endedAt = SDL_GetTicksNS(); printf("fin: %s\n", net.EndReason().c_str()); fflush(stdout); }
			banner = "FIN DE LA PARTIDA\n" + net.EndReason() + "   (Esc = salir)";
			if (SDL_GetTicksNS() - endedAt > 8000000000ULL) quit = true;
		}
		// the buttons the game used, so the other player's show too
		Present(showInputs && st == NetMatch::Playing ? net.LastInputs() : NULL, banner.empty() ? NULL : banner.c_str(),
		        st == NetMatch::Playing ? &stats : NULL, cfg.local);

		next += (Uint64)(frameNs * net.PaceFactor());
		Uint64 now = SDL_GetTicksNS();
		if (next > now) SDL_DelayPrecise(next - now);
		else if (now - next > 5 * frameNs) next = now;
	}
	const NetStats& s = net.Stats();
	printf("resumen: %d cuadros; ping %u ms; delay %d; %d rollbacks (el mas largo %d); desincronizaciones %d\n",
	       s.frame, s.ping, s.delay, s.rollbacks, s.maxRollback, s.desyncs);
	fflush(stdout);
	net.Stop();
	CloseAll();
	return s.desyncs ? 1 : 0;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
	// The ROM folder may have accents: take the arguments as UTF-8, and print UTF-8.
	static std::vector<std::string> utf8;
	static std::vector<char*> args;
	int wargc = 0;
	LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	for (int i = 0; wargv && i < wargc; i++) {
		int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
		std::string a(n > 0 ? n - 1 : 0, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, NULL, NULL);
		utf8.push_back(a);
	}
	for (auto& a : utf8) args.push_back(a.data());
	if (wargv) { LocalFree(wargv); argc = (int)args.size(); argv = args.data(); }
	SetConsoleOutputCP(CP_UTF8);
#endif
	if (argc < 3) {
		fprintf(stderr, "uso: guasa-game check|versus|test|online <carpeta de ROMs> ...\n");
		return 2;
	}
	std::string mode = argv[1];
	if (mode == "check") {
		char report[4000] = "";
		int n = guasa_check_rom(argv[2], report, sizeof(report));
		printf("%s", report);
		return n == 0 ? 0 : 1;
	}
	if (mode == "versus" || mode == "test") return Play(argv[2], mode == "test");
	if (mode == "online") {
		NetConfig cfg;
		if (argc >= 5) {
			cfg.local = atoi(argv[3]) - 1;
			cfg.port = atoi(argv[4]);
		}
		for (int i = 5; i < argc; i++)
			if (!ParseNetArg(argv[i], cfg)) { fprintf(stderr, "opción desconocida: %s\n", argv[i]); return 2; }
		if (argc < 6 || cfg.local < 0 || cfg.local > 1 || (cfg.remote.empty() && (cfg.relay.empty() || cfg.token.size() != 16))) {
			fprintf(stderr, "uso: guasa-game online <carpeta de ROMs> <1|2> <puerto_local> (<ip:puerto> | relay=<host:puerto> token=<32 hex> [relayonly]) [delay=N|auto] [lag=MS] [jitter=MS] [loss=PCT]\n");
			return 2;
		}
		return PlayOnline(argv[2], cfg);
	}
	fprintf(stderr, "modo desconocido: %s\n", mode.c_str());
	return 2;
}
