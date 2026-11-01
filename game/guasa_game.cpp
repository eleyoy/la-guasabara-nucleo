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
//   guasa-game practica <roms>   practice mode (practica.h): the player against a rival
//                                that stands, blocks or repeats a recording
//   guasa-game practica-prueba <roms>   checks practice mode without a window
//
// In the window: F1 sets up a control, F11 or Alt+Enter = full screen.
// Online also: F2 shows the buttons, F5 / F6 = less / more delay, F7 / F8 = mute
// my microphone / the rival (voice chat, handled by the interface), Esc leaves.
// Online, for the interface: "resultado 1|2" on stdout when a game ends, "voz mic"
// and "voz rival" for F7 and F8; on stdin "salir" closes the window (the set is
// over) and "voz estado mic=on|off rival=on|off|nada" updates the voice indicator.
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
#include "practica.h"
#include <atomic>
#include <functional>
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

// Voice chat, as the interface reports it ("voz estado ..."): -1 no voice in this set.
static std::atomic<int> g_voiceMic{ -1 }, g_voiceRival{ -1 };

// Top line in online matches, like Fightcade's: ping, jitter, delay and rollback.
static void DrawStats(const NetStats& st, int local)
{
	SDL_FRect r = GameRect();
	float s = r.h / 450 > 1 ? r.h / 450 : 1;
	char line[200];
	snprintf(line, sizeof(line), "P%d   ping %u ms   jitter %.0f ms   delay %d   rollback %d (max %d)%s%s",
	         local + 1, st.ping, st.jitter, st.delay, st.lastRollback, st.maxRollback,
	         g_voiceMic < 0 ? "" : g_voiceMic ? "   F7 mic: si" : "   F7 mic: NO",
	         g_voiceRival < 0 ? "" : g_voiceRival ? "  F8 rival: se oye" : "  F8 rival: SILENCIADO");
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
		while (std::getline(std::cin, line)) {
			if (line.rfind("salir", 0) == 0) { leave = true; return; }
			if (line.rfind("voz estado", 0) == 0) {
				auto val = [&](const char* key) {
					size_t at = line.find(key);
					if (at == std::string::npos) return -1;
					std::string v = line.substr(at + strlen(key), 3);
					return v.rfind("on", 0) == 0 ? 1 : v.rfind("off", 0) == 0 ? 0 : -1;
				};
				g_voiceMic = val("mic=");
				g_voiceRival = val("rival=");
			}
		}
	}).detach();

	ResultWatcher results;
	const Uint64 frameNs = (Uint64)(1e9 / guasa_fps());   // 59.63 frames per second
	Uint64 next = SDL_GetTicksNS(), endedAt = 0;
	bool quit = false, showInputs = false, wasPlaying = false;
	int shownDelay = -1;
	double pingSum = 0, jitterSum = 0;   // for the summary the hub keeps (average while playing)
	int pingN = 0;
	unsigned pingMax = 0;
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
				case SDL_SCANCODE_F7: printf("voz mic\n"); fflush(stdout); break;
				case SDL_SCANCODE_F8: printf("voz rival\n"); fflush(stdout); break;
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
			if (st == NetMatch::Playing && stats.ping) {
				pingSum += stats.ping; jitterSum += stats.jitter; pingN++;
				if (stats.ping > pingMax) pingMax = stats.ping;
			}
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
	// one line the app passes to the hub, so the admin sees how every set went
	printf("resumen cuadros=%d ping=%.0f pingmax=%u jitter=%.0f delay=%d rollbacks=%d rollbackmax=%d desincronizaciones=%d primera=%d ruta=%s\n",
	       s.frame, pingN ? pingSum / pingN : 0.0, pingMax, pingN ? jitterSum / pingN : 0.0, s.delay, s.rollbacks, s.maxRollback,
	       s.desyncs, s.firstDesync, net.Relayed() ? "servidor" : "directa");
	fflush(stdout);
	net.Stop();
	CloseAll();
	return s.desyncs ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Practice mode (practica.h): the window, the menu and the button history.

// A direction as an arrow in a box (nothing for neutral).
static void Arrow(float cx, float cy, float len, uint16_t in, Uint8 r, Uint8 g, Uint8 b)
{
	float dx = (float)(!!(in & GUASA_RIGHT) - !!(in & GUASA_LEFT)), dy = (float)(!!(in & GUASA_DOWN) - !!(in & GUASA_UP));
	if (!dx && !dy) return;
	float n = SDL_sqrtf(dx * dx + dy * dy);
	dx /= n; dy /= n;
	float tx = cx + dx * len, ty = cy + dy * len;
	SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
	for (float o = -1; o <= 1; o++) {   // three lines: thick enough to read
		SDL_RenderLine(g_ren, cx - dx * len + o * dy, cy - dy * len - o * dx, tx + o * dy, ty - o * dx);
		SDL_RenderLine(g_ren, tx + o, ty, tx - (dx + dy) * len * 0.7f + o, ty - (dy - dx) * len * 0.7f);
		SDL_RenderLine(g_ren, tx + o, ty, tx - (dx - dy) * len * 0.7f + o, ty - (dy + dx) * len * 0.7f);
	}
}

// One column per player: newest on top, how many frames each input lasted, the
// direction as an arrow and the buttons pressed.
static void DrawHistory(const SDL_FRect& r, const std::deque<HistoryEntry>& h, bool right, const char* title)
{
	float s = r.h / 450 > 1 ? r.h / 450 : 1, row = 15 * s, w = 150 * s;
	float x = right ? r.x + r.w - w - 6 * s : r.x + 6 * s, y = r.y + 64 * s;
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 140);
	SDL_FRect box{ x, y - 14 * s, w, row * h.size() + 18 * s };
	SDL_RenderFillRect(g_ren, &box);
	Text(x + 4 * s, y - 11 * s, s, title, 255, 220, 120);
	static const char* names[6] = { "LP", "MP", "HP", "LK", "MK", "HK" };
	for (size_t i = 0; i < h.size(); i++) {
		float ry = y + i * row;
		char n[8];
		snprintf(n, sizeof(n), "%3d", h[i].frames);
		Text(x + 4 * s, ry + 3 * s, s, n, 170, 170, 170);
		Arrow(x + 40 * s, ry + 7 * s, 5 * s, h[i].in, 255, 255, 255);
		for (int b = 0; b < 6; b++)
			if (h[i].in & (GUASA_LP << b)) Text(x + (54 + b * 16) * s, ry + 3 * s, s, names[b], b < 3 ? 120 : 255, b < 3 ? 200 : 140, b < 3 ? 255 : 120);
	}
}

struct MenuItem { std::string text, key; std::function<void(int)> act; };   // act(+1/-1): change it or do it

static void DrawPractice(const Practice& P, const std::vector<MenuItem>* menu, int sel, const std::string& note)
{
	SDL_UpdateTexture(g_tex, NULL, guasa_framebuffer(NULL, NULL, NULL), g_w * 2);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
	SDL_RenderClear(g_ren);
	SDL_FRect r = GameRect();
	SDL_RenderTexture(g_ren, g_tex, NULL, &r);
	float s = r.h / 450 > 1 ? r.h / 450 : 1;
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 160);
	SDL_FRect top{ r.x, r.y, r.w, 14 * s }, bottom{ r.x, r.y + r.h - 14 * s, r.w, 14 * s };
	SDL_RenderFillRect(g_ren, &top);
	SDL_RenderFillRect(g_ren, &bottom);
	Text(r.x + 6 * s, r.y + 3 * s, s, P.Status().c_str(), 255, 220, 120);
	Text(r.x + 6 * s, r.y + r.h - 11 * s, s, "MONEDA o Tab: menu    START: volver a la posicion    Esc: salir", 200, 200, 200);
	if (P.showHistory && P.phase == Practice::Fight) {
		DrawHistory(r, P.history[0], false, "TU");
		if (P.controlRival || P.replay) DrawHistory(r, P.history[1], true, "RIVAL");
	}
	if (!note.empty()) {
		SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 200);
		SDL_FRect b{ r.x + r.w / 2 - 160 * s, r.y + 40 * s, 320 * s, 22 * s };
		SDL_RenderFillRect(g_ren, &b);
		Text(b.x + 8 * s, b.y + 7 * s, s, note.c_str(), 120, 255, 160);
	}
	if (menu) {
		float bs = s * 1.4f, row = 17 * bs, w = 330 * bs, h = row * menu->size() + 34 * bs;
		SDL_FRect box{ r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h };
		SDL_SetRenderDrawColor(g_ren, 10, 10, 18, 235);
		SDL_RenderFillRect(g_ren, &box);
		SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255);
		SDL_RenderRect(g_ren, &box);
		Text(box.x + 12 * bs, box.y + 8 * bs, bs, "PRACTICA - MENU", 255, 46, 136);
		for (size_t i = 0; i < menu->size(); i++) {
			float y = box.y + 26 * bs + i * row;
			if ((int)i == sel) {
				SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255);
				SDL_FRect hl{ box.x + 6 * bs, y - 3 * bs, w - 12 * bs, row - 2 * bs };
				SDL_RenderFillRect(g_ren, &hl);
			}
			Uint8 c = (int)i == sel ? 10 : 230;
			Text(box.x + 12 * bs, y + 1 * bs, bs, (*menu)[i].text.c_str(), c, c, (int)i == sel ? 18 : 230);
			const std::string& k = (*menu)[i].key;
			if (!k.empty()) Text(box.x + w - (12 + 8 * k.size()) * bs, y + 1 * bs, bs, k.c_str(), (int)i == sel ? 10 : 150, (int)i == sel ? 10 : 150, (int)i == sel ? 18 : 170);
		}
		Text(box.x + 12 * bs, box.y + h - 12 * bs, s, "Arriba/abajo: elegir   Golpe: cambiar   Patada: cerrar", 170, 170, 170);
	}
	SDL_RenderPresent(g_ren);
}

// For checking the screen without hands: "captura=<folder>" plays a fixed script
// (pick, walk up, a few hits) and saves practica-pelea.bmp and practica-menu.bmp.
static void SaveWindow(const std::string& path)
{
	SDL_Surface* s = SDL_RenderReadPixels(g_ren, NULL);
	if (s) { SDL_SaveBMP(s, path.c_str()); SDL_DestroySurface(s); }
}

static int PlayPractice(const char* roms, const std::string& shots = "")
{
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) {
		char report[4000] = "";
		guasa_check_rom(roms, report, sizeof(report));
		printf("error: %s\n%s", msg, report);
		return 1;
	}
	guasa_boot_to_select();
	Practice P;
	P.Start();
	if (!OpenWindow("La Guasabara - Practica")) return 1;
	LoadMaps();
	printf("practica\n");
	fflush(stdout);

	std::string note;
	int noteFrames = 0;
	auto say = [&](const std::string& t) { note = t; noteFrames = 120; };
	bool quit = false, menuOpen = false;
	// A second control moves the rival only when asked for in the menu: some
	// controls (the 8BitDo) show up twice, and the rival would copy the player.
	bool secondControl = false;
	int sel = 0;
	uint16_t lastLocal = 0;
	auto yesNo = [](bool b) { return std::string(b ? "si" : "no"); };
	std::vector<MenuItem> menu;
	auto buildMenu = [&]() {
		menu = {
			{ "Volver a la posicion", "START  F8", [&](int) { if (P.LoadPosition()) menuOpen = false; } },
			{ "Guardar esta posicion", "F7", [&](int) { P.SavePosition(); say("Posicion guardada"); } },
			{ std::string("Rival: ") + StanceName(P.stance), "F4", [&](int d) { P.stance = (P.stance + d + kStances) % kStances; P.replay = false; } },
			{ P.rec == Practice::Recording ? "Parar la grabacion" : "Grabar al rival (tu lo mueves)", "F5",
			  [&](int) { P.ToggleRecord(); menuOpen = false; } },
			{ "Repetir la grabacion: " + yesNo(P.replay), "F6", [&](int) { if (!P.tape.empty()) P.replay = !P.replay; } },
			{ "Mover al rival tu mismo: " + yesNo(P.controlRival), "F3", [&](int) { if (P.rec == Practice::NoRec) P.controlRival = !P.controlRival; } },
			{ "Rival con otro control: " + (g_devs.size() < 2 ? std::string("no hay") : yesNo(secondControl)), "",
			  [&](int) { secondControl = !secondControl && g_devs.size() >= 2; } },
			{ "Vida que se recupera: " + yesNo(P.refillLife), "", [&](int) { P.refillLife = !P.refillLife; } },
			{ "Tiempo infinito: " + yesNo(P.freezeClock), "", [&](int) { P.freezeClock = !P.freezeClock; } },
			{ "Super siempre llena: " + yesNo(P.fullSuper), "", [&](int) { P.fullSuper = !P.fullSuper; } },
			{ "Delay como en linea: " + std::to_string(P.delay), "F9", [&](int d) { P.delay = (P.delay + d + 6) % 6; } },
			{ "Historial de botones: " + yesNo(P.showHistory), "F2", [&](int) { P.showHistory = !P.showHistory; } },
			{ "Cambiar de personajes", "F10", [&](int) { P.ChangeCharacters(); menuOpen = false; } },
			{ "Salir de la practica", "Esc", [&](int) { quit = true; } },
		};
	};

	const Uint64 frameNs = (Uint64)(1e9 / guasa_fps());
	Uint64 next = SDL_GetTicksNS();
	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (CaptureEvent(ev)) continue;
			switch (ev.type) {
			case SDL_EVENT_QUIT: quit = true; break;
			case SDL_EVENT_JOYSTICK_ADDED: AddDevice(ev.jdevice.which); break;
			case SDL_EVENT_JOYSTICK_REMOVED: RemoveDevice(ev.jdevice.which); break;
			case SDL_EVENT_KEY_DOWN:
				if (ev.key.repeat) break;
				switch (ev.key.scancode) {
				case SDL_SCANCODE_ESCAPE: if (menuOpen) menuOpen = false; else quit = true; break;
				case SDL_SCANCODE_TAB: menuOpen = !menuOpen; sel = 0; break;
				case SDL_SCANCODE_F1: StartCapture(); break;
				case SDL_SCANCODE_F2: P.showHistory = !P.showHistory; break;
				case SDL_SCANCODE_F3: if (P.rec == Practice::NoRec) P.controlRival = !P.controlRival; break;
				case SDL_SCANCODE_F4: P.stance = (P.stance + 1) % kStances; P.replay = false; say(std::string("Rival: ") + StanceName(P.stance)); break;
				case SDL_SCANCODE_F5: P.ToggleRecord(); say(P.rec == Practice::Recording ? "Grabando: mueve al rival" : "Grabacion lista: el rival la repite"); break;
				case SDL_SCANCODE_F6: if (!P.tape.empty()) P.replay = !P.replay; break;
				case SDL_SCANCODE_F7: P.SavePosition(); say("Posicion guardada"); break;
				case SDL_SCANCODE_F8: P.LoadPosition(); break;
				case SDL_SCANCODE_F9: P.delay = (P.delay + 1) % 6; say("Delay como en linea: " + std::to_string(P.delay)); break;
				case SDL_SCANCODE_F10: P.ChangeCharacters(); break;
				default: break;
				}
				if (ev.key.scancode == SDL_SCANCODE_F11 || (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)))
					SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
				break;
			}
		}

		uint16_t in[2] = { 0, 0 };
		if (!g_cap.active) ReadPlayers(in);
		if (!shots.empty()) {   // the script: pick both, walk up, hit, then the menu
			static int t = 0, fightAt = -1;
			t++;
			in[0] = in[1] = 0;
			if ((t > 30 && t < 34) || (t > 64 && t < 68)) in[0] = GUASA_LP;
			if (P.phase == Practice::Fight && fightAt < 0) fightAt = t;
			int k = fightAt < 0 ? -1 : t - fightAt;
			if (k >= 0 && k < 80) in[0] = GUASA_RIGHT;
			if (k >= 80 && k < 200 && k % 24 < 3) in[0] = k % 48 < 24 ? GUASA_DOWN | GUASA_MK : GUASA_HP;
			if (k >= 200 && k < 215) in[0] = GUASA_DOWN | GUASA_RIGHT;
			if (k == 230) { DrawPractice(P, NULL, 0, "Posicion guardada"); SaveWindow(shots + "/practica-pelea.bmp"); }
			if (k == 232) in[0] = GUASA_COIN;
			if (k == 236) in[0] = GUASA_DOWN;
			if (k == 240) { buildMenu(); DrawPractice(P, &menu, 1, ""); SaveWindow(shots + "/practica-menu.bmp"); quit = true; }
		}
		if (g_devs.size() < 2) secondControl = false;
		int second = secondControl ? in[1] : -1;
		uint16_t local = secondControl ? in[0] : in[0] | in[1], pressed = local & ~lastLocal;
		lastLocal = local;

		// MONEDA opens and closes the menu; START goes back to the saved position
		if (pressed & GUASA_COIN) { menuOpen = !menuOpen; sel = 0; }
		if (menuOpen) {
			buildMenu();
			if (pressed & GUASA_UP) sel = (sel + (int)menu.size() - 1) % (int)menu.size();
			if (pressed & GUASA_DOWN) sel = (sel + 1) % (int)menu.size();
			if (pressed & (GUASA_LK | GUASA_MK | GUASA_HK)) menuOpen = false;
			else if (pressed & (GUASA_LP | GUASA_MP | GUASA_HP | GUASA_START | GUASA_RIGHT)) menu[sel].act(1);
			else if (pressed & GUASA_LEFT) menu[sel].act(-1);
			if (menuOpen) buildMenu();   // its texts after the change
			DrawPractice(P, menuOpen ? &menu : NULL, sel, "");
		} else {
			if ((pressed & GUASA_START) && P.phase == Practice::Fight && !P.LoadPosition()) say("Todavia no hay posicion guardada");
			uint16_t out[2];
			P.Inputs(local & ~(GUASA_START | GUASA_COIN), second, out);
			guasa_run_frame(out, 1);
			PushAudio();
			P.AfterFrame(out);
			if (noteFrames > 0 && --noteFrames == 0) note.clear();
			std::string banner = g_cap.active ? CaptureBanner() : "";
			if (!banner.empty()) Present(NULL, banner.c_str());
			else DrawPractice(P, NULL, 0, note);
		}

		next += frameNs;
		Uint64 now = SDL_GetTicksNS();
		if (next > now) SDL_DelayPrecise(next - now);
		else if (now - next > 5 * frameNs) next = now;
	}
	CloseAll();
	return 0;
}

// practica-prueba: the practice mode's own checks, without a window. One line
// per check (PASA / FALLA); exit code 0 only if all pass.
static int PracticeSelfTest(const char* roms)
{
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) { printf("error: %s\n", msg); return 1; }
	guasa_boot_to_select();
	Practice P;
	P.Start();
	int fails = 0;
	auto check = [&](bool ok, const char* what, const std::string& detail = "") {
		printf("%s  %s%s\n", ok ? "PASA " : "FALLA", what, detail.empty() ? "" : ("  (" + detail + ")").c_str());
		fails += !ok;
	};
	auto frame = [&](uint16_t local) { uint16_t out[2]; P.Inputs(local, -1, out); guasa_run_frame(out, 0); P.AfterFrame(out); return out[0]; };
	auto run = [&](int n, uint16_t local) { for (int i = 0; i < n; i++) frame(local); };
	auto life2 = [] { return (int)(int16_t)ReadWord(kP2 + kLife); };
	auto x2 = [] { return (int)ReadWord(kP2 + kX); };
	auto char2 = [] { return (int)guasa_read_ram(kP2 + 0x391); };
	// the rival's cursor moves one way, then the rival is picked (the button that
	// picked the player's character, still held, must not pick it)
	auto pickRival = [&](uint16_t dir) {
		run(4, dir); run(10, 0); run(4, GUASA_LP);
		int f = 0;
		while (P.phase != Practice::Fight && f++ < 4000) frame(0);
		return f;
	};

	run(30, 0); run(4, GUASA_LP); run(30, 0);
	check(P.phase == Practice::PickRival, "con el primer golpe se elige tu personaje y sigue el del rival");
	int f = pickRival(GUASA_RIGHT), rivalRight = char2();
	check(P.phase == Practice::Fight && P.HasPosition(), "empieza la pelea y queda guardada la posicion", std::to_string(f) + " cuadros");

	// an attack against a rival that blocks, and against one that stands
	auto attack = [&](int stance) {
		P.LoadPosition();
		P.stance = stance;
		int least = life2();
		run(100, GUASA_RIGHT);   // up against the rival
		for (int k = 0; k < 12; k++) { run(3, GUASA_HP); run(20, 0); least = std::min(least, life2()); }
		return least;
	};
	int blocked = attack(kBlock), hit = attack(kStand);
	check(blocked == kFullLife, "el rival que bloquea no pierde vida", "vida minima " + std::to_string(blocked));
	check(hit < kFullLife, "el rival de pie si recibe los golpes", "vida minima " + std::to_string(hit));
	run(90, 0);
	check(life2() == kFullLife, "la vida vuelve a llenarse", std::to_string(life2()));
	check(guasa_read_ram(kClock) == 0x99, "el reloj se queda en 99");

	// record the rival walking forward, then it repeats it from the same position
	P.LoadPosition();
	int start = x2();
	P.ToggleRecord();
	run(30, GUASA_LEFT);
	int walked = x2();
	P.ToggleRecord();
	check(P.tape.size() == 30 && P.replay, "la grabacion tiene 30 cuadros y el rival la repite", std::to_string(P.tape.size()));
	P.LoadPosition();
	check(x2() == start, "volver a la posicion devuelve al rival a su lugar");
	run(30, 0);
	check(x2() == walked, "el rival repite lo grabado", std::to_string(start) + " -> " + std::to_string(walked) + ", repetido " + std::to_string(x2()));

	// buttons late like online
	P.replay = false;
	P.delay = 3;
	int late = -1;
	for (int i = 0; i < 10 && late < 0; i++) if (frame(GUASA_UP) & GUASA_UP) late = i;
	check(late == 3, "con delay 3 el boton llega 3 cuadros despues", std::to_string(late));
	P.delay = 0;

	check(!P.history[0].empty() && (P.history[0].front().in & GUASA_UP), "el historial muestra el ultimo boton");
	P.ChangeCharacters();
	check(P.phase == Practice::PickMine && !P.HasPosition(), "cambiar de personajes vuelve a la seleccion");
	run(30, 0); run(4, GUASA_LP); run(30, 0);
	pickRival(GUASA_LEFT);
	int rivalLeft = char2();
	check(P.phase == Practice::Fight && rivalLeft != rivalRight, "el rival es el que elige el jugador, no el del cursor",
	      "derecha " + std::to_string(rivalRight) + ", izquierda " + std::to_string(rivalLeft));
	guasa_exit();
	return fails ? 1 : 0;
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
	if (mode == "practica") return PlayPractice(argv[2], argc > 3 && strncmp(argv[3], "captura=", 8) == 0 ? argv[3] + 8 : "");
	if (mode == "practica-prueba") return PracticeSelfTest(argv[2]);
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
