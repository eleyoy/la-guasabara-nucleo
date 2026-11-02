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
// and "voz rival" for F7 and F8, "mensaje <text>" for a message to the rival (T);
// on stdin "salir" closes the window (the set is over), "voz estado mic=on|off
// rival=on|off|nada" updates the voice indicator, "mensaje <name>\t<text>"
// shows a message (":id:" is one of the league's emojis), "rapido <0-9> <text>"
// sets what keys 1 to 0 send (0 = key 1 ... 9 = key 0) and "jugador <0|1>
// <rank|-> <name>" names each side, with its rank's badge.
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
#include <mutex>
#include <deque>
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

// A file in %APPDATA%\La Guasabara\juego
static std::string PrefFile(const char* name)
{
	char* dir = SDL_GetPrefPath("La Guasabara", "juego");
	std::string p = std::string(dir ? dir : "") + name;
	SDL_free(dir);
	return p;
}

static std::string MapsPath() { return PrefFile("controles.txt"); }

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
// keyboard = false leaves the keyboard out (it's typing a message).
static bool KeyIsButton(SDL_Scancode sc);

static void ReadPlayers(uint16_t in[2], bool keyboard = true)
{
	auto k = g_maps.find("teclado");
	const Mapping& keys = k != g_maps.end() ? k->second : KeyboardDefault();
	in[0] = in[1] = 0;
	if (keyboard)
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

static std::function<void(const SDL_FRect&)> g_overlay;   // drawn last (messages in online sets)

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
	if (g_overlay) g_overlay(r);
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

// Whether a key is one of player 1's buttons on the keyboard (then T and P don't
// open messages or pause: F4 and F3 do).
static bool KeyIsButton(SDL_Scancode sc)
{
	auto k = g_maps.find("teclado");
	const Mapping& keys = k != g_maps.end() ? k->second : KeyboardDefault();
	for (int i = 0; i < kActions; i++) if (keys.b[i].kind == 'k' && keys.b[i].index == (int)sc) return true;
	return false;
}

// ---------------------------------------------------------------------------
// Messages during the set: the league's emojis, quick phrases or a few words.

static const char* kEmojis[] = { "aguantala", "calma-fiera", "coje-tu-palo", "ggs", "palomo", "que-e-leon",
	"se-cayo-el-sistema", "vamo-al-game", "y-ahora" };
static const char* kPhrases[] = { "GG", "Espera, por favor", "Una mas?", "Ya vuelvo", "Buena esa" };

// The window's letters are plain ASCII: accents and the like are written without them.
static std::string Plain(const std::string& u)
{
	static const struct { const char* from; char to; } fold[] = {
		{ "\xC3\xA1", 'a' }, { "\xC3\xA9", 'e' }, { "\xC3\xAD", 'i' }, { "\xC3\xB3", 'o' }, { "\xC3\xBA", 'u' }, { "\xC3\xBC", 'u' },
		{ "\xC3\xB1", 'n' }, { "\xC3\x81", 'A' }, { "\xC3\x89", 'E' }, { "\xC3\x8D", 'I' }, { "\xC3\x93", 'O' }, { "\xC3\x9A", 'U' },
		{ "\xC3\x91", 'N' }, { "\xC2\xA1", '!' }, { "\xC2\xBF", '?' },
	};
	std::string out;
	for (size_t i = 0; i < u.size();) {
		bool done = false;
		for (auto& f : fold)
			if (!u.compare(i, 2, f.from)) { out += f.to; i += 2; done = true; break; }
		if (done) continue;
		unsigned char c = (unsigned char)u[i];
		if (c < 0x80) { if (c >= 32) out += (char)c; i++; }
		else { i++; while (i < u.size() && ((unsigned char)u[i] & 0xC0) == 0x80) i++; }   // anything else: left out
	}
	return out;
}

// An emoji's picture, from the "emojis" folder next to the game (loaded once).
static SDL_Texture* EmojiTexture(const std::string& id)
{
	static std::map<std::string, SDL_Texture*> cache;
	auto it = cache.find(id);
	if (it != cache.end()) return it->second;
	SDL_Texture* t = NULL;
	if (SDL_Surface* sf = SDL_LoadPNG((std::string(SDL_GetBasePath()) + "emojis/" + id + ".png").c_str())) {
		t = SDL_CreateTextureFromSurface(g_ren, sf);
		SDL_DestroySurface(sf);
	}
	cache[id] = t;
	return t;
}

// The emoji's id when the text is just ":id:" of one of them, else "".
static std::string EmojiOf(const std::string& text)
{
	if (text.size() < 3 || text.front() != ':' || text.back() != ':') return "";
	std::string id = text.substr(1, text.size() - 2);
	for (const char* e : kEmojis) if (id == e) return id;
	return "";
}

// A name's color, the same one the app gives it in the chats: soft colors, a few
// of them, so some names share one (as in Fightcade). From the UTF-8 bytes.
static void NameColor(const std::string& name, Uint8& r, Uint8& g, Uint8& b)
{
	static const Uint8 palette[][3] = { { 0xE9, 0xA6, 0xA6 }, { 0xE8, 0xC0, 0x7D }, { 0xA8, 0xD0, 0x8D }, { 0x7F, 0xC8, 0xC2 },
		{ 0x8F, 0xB3, 0xE8 }, { 0xB7, 0xA3, 0xE3 }, { 0xE3, 0xA3, 0xCF }, { 0xC9, 0xB7, 0x9C }, { 0x9C, 0xC9, 0xE3 }, { 0xD4, 0xD4, 0x8A } };
	uint32_t h = 0;
	for (unsigned char c : name) h = h * 31 + c;
	const Uint8* p = palette[h % 10];
	r = p[0]; g = p[1]; b = p[2];
}

// Each side's player, as the app says: name and rank (empty: no rank yet).
static std::mutex g_namesLock;
static std::string g_names[2], g_ranks[2];

// A rank's badge, from the "rangos" folder next to the game (loaded once).
static SDL_Texture* RankTexture(const std::string& rank)
{
	static std::map<std::string, SDL_Texture*> cache;
	auto it = cache.find(rank);
	if (it != cache.end()) return it->second;
	std::string file;
	for (char c : rank) file += (char)SDL_tolower((unsigned char)c);
	SDL_Texture* t = NULL;
	if (SDL_Surface* sf = SDL_LoadPNG((std::string(SDL_GetBasePath()) + "rangos/" + file + ".png").c_str())) {
		t = SDL_CreateTextureFromSurface(g_ren, sf);
		SDL_DestroySurface(sf);
	}
	cache[rank] = t;
	return t;
}

// Under the top line: player 1's name and badge on the left, player 2's on the right.
static void DrawNames(const SDL_FRect& r)
{
	std::string names[2], ranks[2];
	{
		std::lock_guard<std::mutex> lock(g_namesLock);
		for (int p = 0; p < 2; p++) { names[p] = g_names[p]; ranks[p] = g_ranks[p]; }
	}
	float s = r.h / 450 > 1 ? r.h / 450 : 1, h = 22 * s, y = r.y + 15 * s, ts = s * 1.25f;
	for (int p = 0; p < 2; p++) {
		if (names[p].empty()) continue;
		std::string n = Plain(names[p]);
		float w = h + 6 * s + n.size() * 8 * ts + 12 * s;
		SDL_FRect box{ p ? r.x + r.w - w - 4 * s : r.x + 4 * s, y, w, h };
		SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 140);
		SDL_RenderFillRect(g_ren, &box);
		SDL_FRect badge{ box.x + 3 * s, box.y + 1 * s, h - 2 * s, h - 2 * s };
		if (SDL_Texture* t = ranks[p].empty() ? NULL : RankTexture(ranks[p])) SDL_RenderTexture(g_ren, t, NULL, &badge);
		else Text(badge.x + 3 * s, badge.y + 3 * s, s, "?", 170, 170, 170);
		Uint8 cr, cg, cb;
		NameColor(names[p], cr, cg, cb);
		Text(box.x + h + 6 * s, box.y + 6 * s, ts, n.c_str(), cr, cg, cb);
	}
}

// Keys 1 to 0: quick messages, as the app says (chosen in Mi perfil).
static std::mutex g_quickLock;
static std::string g_quick[10];

// The quick slot of a key: top-row 1..0 (unless it's a game button) or the keypad's; -1 none.
static int QuickSlot(SDL_Scancode sc)
{
	if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return KeyIsButton(sc) ? -1 : sc - SDL_SCANCODE_1;
	if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_0) return sc - SDL_SCANCODE_KP_1;
	return -1;
}

struct ShownMessage { std::string from, text; Uint64 at; int side; };   // side: 0 player 1, 1 player 2

// The last messages, each for 6 seconds, on the side of who wrote it: player 1's
// on the left, player 2's on the right.
static void DrawMessages(const SDL_FRect& r, std::deque<ShownMessage>& list)
{
	Uint64 now = SDL_GetTicksNS();
	while (!list.empty() && now - list.front().at > 6000000000ULL) list.pop_front();
	float s = r.h / 450 > 1 ? r.h / 450 : 1, ys[2] = { r.y + 42 * s, r.y + 42 * s };
	for (auto& m : list) {
		float& y = ys[m.side ? 1 : 0];
		std::string id = EmojiOf(m.text), text = id.empty() ? Plain(m.text) : "";
		float big = id.empty() ? 0 : 56 * s, w = SDL_max(Plain(m.from).size() * 8 * s, text.size() * 8 * s) + big + 16 * s;
		float h = SDL_max(big, 22 * s) + 8 * s;
		SDL_FRect box{ m.side ? r.x + r.w - w - 8 * s : r.x + 8 * s, y, w, h };
		SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(g_ren, 10, 10, 18, 200);
		SDL_RenderFillRect(g_ren, &box);
		SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255);
		SDL_FRect bar{ box.x, box.y, 3 * s, h };
		SDL_RenderFillRect(g_ren, &bar);
		Uint8 nr, ng, nb;
		NameColor(m.from, nr, ng, nb);
		Text(box.x + 8 * s, box.y + 5 * s, s, Plain(m.from).c_str(), nr, ng, nb);
		if (!id.empty()) {
			if (SDL_Texture* t = EmojiTexture(id)) { SDL_FRect d{ box.x + w - big - 4 * s, box.y + 4 * s, big, big }; SDL_RenderTexture(g_ren, t, NULL, &d); }
		} else Text(box.x + 8 * s, box.y + 16 * s, s, text.c_str(), 240, 240, 240);
		y += h + 6 * s;
	}
}

// The panel to send one (T): 9 emojis, the quick phrases and a line to type.
struct MessagePanel {
	bool open = false, justOpened = false;   // justOpened: the key that opened it isn't typed
	int sel = 0;                 // 0-8 emojis, 9-13 phrases, 14 the line
	std::string typed;
	static constexpr int kCount = 9 + 5 + 1;
	struct Layout { SDL_FRect box, items[kCount]; float s; };
	static Layout Lay(const SDL_FRect& r)
	{
		Layout L;
		L.s = r.h / 450 > 1 ? r.h / 450 : 1;
		float s = L.s, cell = 52 * s, w = 3 * cell + 230 * s, h = 3 * cell + 86 * s;
		L.box = { r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h };
		for (int i = 0; i < 9; i++) L.items[i] = { L.box.x + 10 * s + (i % 3) * cell, L.box.y + 24 * s + (i / 3) * cell, cell - 4 * s, cell - 4 * s };
		for (int i = 0; i < 5; i++) L.items[9 + i] = { L.box.x + 3 * cell + 20 * s, L.box.y + 24 * s + i * 26 * s, 200 * s, 22 * s };
		L.items[14] = { L.box.x + 10 * s, L.box.y + h - 52 * s, w - 20 * s, 22 * s };
		return L;
	}
	// What's under (x, y): an item, -1 elsewhere in the panel, -2 outside it.
	int Hit(const SDL_FRect& r, float x, float y) const
	{
		Layout L = Lay(r);
		SDL_FPoint p{ x, y };
		for (int i = 0; i < kCount; i++) if (SDL_PointInRectFloat(&p, &L.items[i])) return i;
		return SDL_PointInRectFloat(&p, &L.box) ? -1 : -2;
	}
	std::string Chosen() const { return sel < 9 ? std::string(":") + kEmojis[sel] + ":" : sel < 14 ? kPhrases[sel - 9] : typed; }
	void Draw(const SDL_FRect& r) const
	{
		Layout L = Lay(r);
		float s = L.s;
		SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(g_ren, 10, 10, 18, 235);
		SDL_RenderFillRect(g_ren, &L.box);
		SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255);
		SDL_RenderRect(g_ren, &L.box);
		Text(L.box.x + 10 * s, L.box.y + 8 * s, s, "MENSAJE AL RIVAL   (Enter: mandar   Esc: cerrar)", 255, 46, 136);
		for (int i = 0; i < kCount; i++) {
			const SDL_FRect& it = L.items[i];
			if (i == sel) { SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255); SDL_FRect hl{ it.x - 2 * s, it.y - 2 * s, it.w + 4 * s, it.h + 4 * s }; SDL_RenderFillRect(g_ren, &hl); }
			SDL_SetRenderDrawColor(g_ren, 25, 25, 36, 255);
			SDL_RenderFillRect(g_ren, &it);
			if (i < 9) {
				if (SDL_Texture* t = EmojiTexture(kEmojis[i])) SDL_RenderTexture(g_ren, t, NULL, &it);
			} else {
				std::string t = i < 14 ? kPhrases[i - 9] : (typed.empty() ? "Escribe aqui..." : Plain(typed) + "_");
				Uint8 c = i == 14 && typed.empty() ? 140 : 235;
				Text(it.x + 6 * s, it.y + 7 * s, s, t.c_str(), c, c, c);
			}
		}
		Text(L.box.x + 10 * s, L.box.y + L.box.h - 18 * s, s, "Sin abrir esto: teclas 1 a 0 = mensajes rapidos (Mi perfil)", 170, 170, 170);
	}
};

static void SaveWindow(const std::string& path);

// shots: "captura=<folder>" (checking the screen without hands): once playing, a
// message arrives, then the panel opens, then the pause; a picture of each.
static int PlayOnline(const char* roms, const NetConfig& cfg, const std::string& shots = "")
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
	static std::mutex inboxLock;
	static std::vector<ShownMessage> inbox;   // messages from the stdin thread
	std::thread([] {
		std::string line;
		while (std::getline(std::cin, line)) {
			if (line.rfind("salir", 0) == 0) { leave = true; return; }
			if (line.rfind("jugador ", 0) == 0 && line.size() > 10 && (line[8] == '0' || line[8] == '1')) {   // "jugador <0|1> <rank|-> <name>"
				size_t sp = line.find(' ', 10);
				if (sp != std::string::npos) {
					std::lock_guard<std::mutex> lock(g_namesLock);
					std::string rank = line.substr(10, sp - 10);
					g_ranks[line[8] - '0'] = rank == "-" ? "" : rank;
					g_names[line[8] - '0'] = line.substr(sp + 1);
				}
			}
			if (line.rfind("rapido ", 0) == 0 && line.size() > 8 && line[7] >= '0' && line[7] <= '9') {   // "rapido <n> <text>"
				std::lock_guard<std::mutex> lock(g_quickLock);
				g_quick[line[7] - '0'] = line.size() > 9 ? line.substr(9) : "";
			}
			if (line.rfind("mensaje ", 0) == 0) {   // "mensaje <name>\t<text>"
				size_t tab = line.find('\t', 8);
				std::lock_guard<std::mutex> lock(inboxLock);
				if (tab != std::string::npos) inbox.push_back({ line.substr(8, tab - 8), line.substr(tab + 1), 0, 0 });
			}
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
	MessagePanel panel;
	NetMatch::Status st0 = NetMatch::Waiting;   // last frame's
	std::deque<ShownMessage> shownMsgs;
	std::string note;            // a short notice in the middle (the pause only in casual...)
	Uint64 noteAt = 0;
	bool pauseAsk = false;       // P (or START held): ask for the pause on the next frame
	int startHeld = 0;
	uint16_t lastPad = 0;
	auto openPanel = [&](bool open) {
		panel.open = open;
		panel.justOpened = open;
		panel.typed.clear();
		if (open) SDL_StartTextInput(g_win); else SDL_StopTextInput(g_win);
	};
	auto sendMessage = [&](const std::string& text) {
		std::string clean;
		for (char c : text) if (c != '\n' && c != '\r') clean += c;
		if (clean.empty()) return;
		clean = clean.substr(0, 120);
		printf("mensaje %s\n", clean.c_str());
		fflush(stdout);
		shownMsgs.push_back({ "Tu", clean, SDL_GetTicksNS(), cfg.local });
		openPanel(false);
	};
	auto askPause = [&]() {
		if (!cfg.pause) { note = "La pausa solo existe en casual"; noteAt = SDL_GetTicksNS(); return; }
		pauseAsk = true;
	};
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
			case SDL_EVENT_TEXT_INPUT:
				if (panel.open && !panel.justOpened && panel.typed.size() < 120) { panel.typed += ev.text.text; panel.sel = 14; }
				break;
			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
				if (panel.open) {
					SDL_ConvertEventToRenderCoordinates(g_ren, &ev);
					bool moved = ev.type == SDL_EVENT_MOUSE_MOTION;
					int hit = panel.Hit(GameRect(), moved ? ev.motion.x : ev.button.x, moved ? ev.motion.y : ev.button.y);
					if (hit >= 0) panel.sel = hit;
					if (!moved && hit == -2) openPanel(false);
					else if (!moved && hit >= 0 && hit < 14) sendMessage(panel.Chosen());
				}
				break;
			case SDL_EVENT_KEY_DOWN:
				if (panel.open) {   // the panel takes the keyboard while it's open
					switch (ev.key.scancode) {
					case SDL_SCANCODE_ESCAPE: openPanel(false); break;
					case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: sendMessage(panel.Chosen()); break;
					case SDL_SCANCODE_BACKSPACE:
						while (!panel.typed.empty() && ((unsigned char)panel.typed.back() & 0xC0) == 0x80) panel.typed.pop_back();
						if (!panel.typed.empty()) panel.typed.pop_back();
						break;
					case SDL_SCANCODE_LEFT: if (panel.sel > 0 && panel.sel != 14) panel.sel--; break;
					case SDL_SCANCODE_RIGHT: if (panel.sel < 13) panel.sel++; break;
					case SDL_SCANCODE_UP: panel.sel = panel.sel == 14 ? 13 : panel.sel >= 9 ? (panel.sel > 9 ? panel.sel - 1 : 8) : (panel.sel >= 3 ? panel.sel - 3 : panel.sel); break;
					case SDL_SCANCODE_DOWN: panel.sel = panel.sel < 6 ? panel.sel + 3 : panel.sel < 9 ? 14 : SDL_min(panel.sel + 1, 14); break;
					default: break;
					}
					break;
				}
				switch (ev.key.scancode) {
				case SDL_SCANCODE_ESCAPE: quit = true; break;
				// T and P, unless they are buttons of the game for this player; F4 and F3 always
				case SDL_SCANCODE_T: if (!KeyIsButton(SDL_SCANCODE_T)) openPanel(true); break;
				case SDL_SCANCODE_F4: openPanel(true); break;
				case SDL_SCANCODE_P: if (!KeyIsButton(SDL_SCANCODE_P)) askPause(); break;
				case SDL_SCANCODE_F3: case SDL_SCANCODE_PAUSE: askPause(); break;
				case SDL_SCANCODE_1: case SDL_SCANCODE_2: case SDL_SCANCODE_3: case SDL_SCANCODE_4: case SDL_SCANCODE_5:
				case SDL_SCANCODE_6: case SDL_SCANCODE_7: case SDL_SCANCODE_8: case SDL_SCANCODE_9: case SDL_SCANCODE_0:
				case SDL_SCANCODE_KP_1: case SDL_SCANCODE_KP_2: case SDL_SCANCODE_KP_3: case SDL_SCANCODE_KP_4: case SDL_SCANCODE_KP_5:
				case SDL_SCANCODE_KP_6: case SDL_SCANCODE_KP_7: case SDL_SCANCODE_KP_8: case SDL_SCANCODE_KP_9: case SDL_SCANCODE_KP_0:
					if (!ev.key.repeat) {   // a quick message: sent at once
						int slot = QuickSlot(ev.key.scancode);
						std::string text;
						if (slot >= 0) { std::lock_guard<std::mutex> lock(g_quickLock); text = g_quick[slot]; }
						if (!text.empty()) sendMessage(text);
					}
					break;
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

		panel.justOpened = false;
		{
			std::lock_guard<std::mutex> lock(inboxLock);
			for (auto& m : inbox) shownMsgs.push_back({ m.from, m.text, SDL_GetTicksNS(), 1 - cfg.local });   // the rival's side
			inbox.clear();
		}
		while (shownMsgs.size() > 6) shownMsgs.pop_front();
		uint16_t in[2] = { 0, 0 };
		// this side plays with player 1's controls; while the message panel is open
		// the keyboard only types (a key that is also a button mustn't send or close it)
		if (!g_cap.active) ReadPlayers(in, !panel.open);
		uint16_t pad = in[0], pressed = pad & ~lastPad;
		lastPad = pad;
		if (panel.open) {   // with the controller: directions move, a punch sends, a kick closes
			if (pressed & GUASA_LEFT) panel.sel = SDL_max(panel.sel - 1, 0);
			if (pressed & GUASA_RIGHT) panel.sel = SDL_min(panel.sel + 1, 13);
			if (pressed & GUASA_UP) panel.sel = panel.sel >= 3 && panel.sel < 9 ? panel.sel - 3 : panel.sel;
			if (pressed & GUASA_DOWN) panel.sel = panel.sel < 6 ? panel.sel + 3 : panel.sel;
			if (pressed & (GUASA_LP | GUASA_MP | GUASA_HP)) sendMessage(panel.Chosen());
			else if (pressed & (GUASA_LK | GUASA_MK | GUASA_HK)) openPanel(false);
			in[0] = 0;   // the fight doesn't get these buttons
		}
		// START held a second asks for the pause too (for those on a controller)
		startHeld = (in[0] & GUASA_START) ? startHeld + 1 : 0;
		if (startHeld == 60) askPause();
		if (!shots.empty() && st0 == NetMatch::Playing) {   // the script
			static int t = 0;
			t++;
			if (t == 60) {
				{ std::lock_guard<std::mutex> lock(g_namesLock); g_names[0] = "yoy"; g_ranks[0] = "Naboria"; g_names[1] = "ELGINGIRIANO"; g_ranks[1] = ""; }
				std::lock_guard<std::mutex> lock(inboxLock);
				inbox.push_back({ "ELGINGIRIANO", ":palomo:", 0, 0 });
				inbox.push_back({ "ELGINGIRIANO", "Que tal, mi hermano? Una mas!", 0, 0 });
				shownMsgs.push_back({ "yoy", ":ggs:", SDL_GetTicksNS(), cfg.local });
			}
			if (t == 90) SaveWindow(shots + "/online-mensajes.bmp");
			if (t == 100) { openPanel(true); panel.sel = 4; }
			if (t == 110) SaveWindow(shots + "/online-panel.bmp");
			if (t == 120) { openPanel(false); askPause(); }
			if (t == 170) SaveWindow(shots + "/online-pausa.bmp");
			if (t == 180) quit = true;
		}
		uint16_t local = in[0];
		if (pauseAsk) { local |= kPauseBit; pauseAsk = false; }   // one frame is enough: both games see it
		bool shown = false;
		NetMatch::Status st = net.Step(local, true, &shown);
		st0 = st;
		const NetStats& stats = net.Stats();
		if (shown && net.Pause().phase == 0) {   // paused: no sound (it would repeat)
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
		const PauseState& ps = net.Pause();
		if (g_cap.active) banner = CaptureBanner();
		else if (st == NetMatch::Waiting) banner = "CONECTANDO\n" + net.RouteText() + "   (Esc = salir)";
		else if (ps.phase == 1) {
			int left = (int)((5 * 60 * guasa_fps() - ps.frames) / guasa_fps());
			char b[160];
			snprintf(b, sizeof(b), "PAUSA\n%s. Quedan %d:%02d. P o START: seguir", ps.by == cfg.local ? "Pausaste" : "Tu rival pauso", left / 60, left % 60);
			banner = b;
		} else if (ps.phase == 2) banner = "VOLVEMOS EN " + std::to_string((int)(ps.frames / guasa_fps()) + 1) + "\n ";
		else if (st == NetMatch::Ended) {
			if (!endedAt) { endedAt = SDL_GetTicksNS(); printf("fin: %s\n", net.EndReason().c_str()); fflush(stdout); }
			banner = "FIN DE LA PARTIDA\n" + net.EndReason() + "   (Esc = salir)";
			if (SDL_GetTicksNS() - endedAt > 8000000000ULL) quit = true;
		}
		g_overlay = [&](const SDL_FRect& r) {
			if (st == NetMatch::Playing || st == NetMatch::Ended) DrawNames(r);
			DrawMessages(r, shownMsgs);
			if (panel.open) panel.Draw(r);
			if (!note.empty() && SDL_GetTicksNS() - noteAt < 2500000000ULL) {
				float s = r.h / 450 > 1 ? r.h / 450 : 1;
				SDL_FRect b{ r.x + r.w / 2 - 150 * s, r.y + 40 * s, 300 * s, 22 * s };
				SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 200);
				SDL_RenderFillRect(g_ren, &b);
				Text(b.x + 8 * s, b.y + 7 * s, s, note.c_str(), 255, 220, 120);
			}
		};
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
	g_overlay = nullptr;
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

// One row of the practice menu: what it is, its value (if it has one) and its key.
// act(+1 / -1): do it, or change the value forward / back. A header only names a group.
// The frame meter (practica.h): two rows of colored frames above the bottom bar,
// like SFV: green startup, red active, blue recovery, yellow hit or block stun,
// purple knocked down, grey free. Above them, the attack's numbers and the advantage.
static void DrawFrameMeter(const SDL_FRect& r, const FrameMeter& m)
{
	if (m.row[0].empty()) return;
	float s = r.h / 450 > 1 ? r.h / 450 : 1;
	static const Uint8 colors[][3] = {
		{ 70, 74, 90 },    // free
		{ 60, 210, 110 },  // startup
		{ 240, 60, 70 },   // active
		{ 70, 130, 255 },  // recovery
		{ 250, 210, 60 },  // hit stun
		{ 250, 170, 40 },  // block stun
		{ 170, 90, 230 },  // knocked down
		{ 120, 190, 210 }, // in the air
		{ 255, 46, 136 },  // throw
		{ 35, 35, 45 },    // frozen (hitstop, only one of the two)
	};
	size_t n = m.row[0].size();
	// narrow and see-through, so it covers little of the fight: at most ~40% of the
	// width (long exchanges get thinner frames), on a faint background
	float ts = s * 0.85f, label = 30 * s, h = 7 * s;
	float cell = SDL_min(4 * s, (r.w * 0.40f - label) / SDL_max((float)n, 1.0f));
	float x0 = r.x + 8 * s + label, y0 = r.y + r.h - 14 * s - 2 * h - 44 * s;   // above the super meter
	char nums[64] = "", adv[24] = "";
	if (m.done && m.who >= 0 && m.active)
		snprintf(nums, sizeof(nums), "Inicio %d  Activo %d  Recup. %d%s", m.startup, m.active, m.recovery, m.who ? " (rival)" : "");
	if (m.done && m.hasAdvantage) snprintf(adv, sizeof(adv), "%+d  ", m.advantage);
	float textW = (strlen(adv) + strlen(nums)) * 8 * ts;
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 90);
	SDL_FRect bg{ r.x + 4 * s, y0 - 12 * s, SDL_max(label + cell * n + 10 * s, textW + 10 * s), 2 * h + 18 * s };
	SDL_RenderFillRect(g_ren, &bg);
	for (int p = 0; p < 2; p++) {
		float y = y0 + p * (h + 2 * s);
		Text(r.x + 8 * s, y, ts, p ? "RIV" : "TU", 220, 220, 220);
		for (size_t i = 0; i < n; i++) {
			const Uint8* c = colors[m.row[p][i] < 10 ? m.row[p][i] : 0];
			SDL_SetRenderDrawColor(g_ren, c[0], c[1], c[2], 190);
			SDL_FRect f{ x0 + i * cell, y, SDL_max(cell - (cell > 2 * s ? 1 : 0), 1.0f), h };
			SDL_RenderFillRect(g_ren, &f);
		}
	}
	if (adv[0]) {   // the advantage first and in color: green ahead, red behind
		Uint8 g = m.advantage > 0 ? 255 : m.advantage < 0 ? 90 : 230, rr = m.advantage < 0 ? 255 : m.advantage > 0 ? 120 : 230;
		Text(r.x + 8 * s, y0 - 10 * s, ts, adv, rr, g, m.advantage > 0 ? 140 : 90);
	}
	Text(r.x + 8 * s + strlen(adv) * 8 * ts, y0 - 10 * s, ts, nums, 225, 225, 225);
}

struct MenuItem {
	std::string label, value, key;
	std::function<void(int)> act;
	bool header = false;
};
static MenuItem Header(const char* t) { MenuItem m; m.label = t; m.header = true; return m; }

// Where the menu goes on the screen: the same numbers draw it and find what the mouse points at.
struct MenuLayout { SDL_FRect box; float bs, row, top; };
static MenuLayout LayMenu(const SDL_FRect& r, size_t n)
{
	float s = r.h / 450 > 1 ? r.h / 450 : 1;
	float bs = SDL_min(s * 1.4f, (r.h - 30 * s) / (17.0f * n + 48));   // smaller if it doesn't fit
	float row = 17 * bs, w = SDL_min(380 * bs, r.w - 20 * s), h = row * n + 48 * bs;
	return { { r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h }, bs, row, r.y + (r.h - h) / 2 + 26 * bs };
}
// The row under (x, y): its index, -1 for a header or the title, -2 outside the menu.
static int MenuHit(const MenuLayout& L, const std::vector<MenuItem>& m, float x, float y)
{
	if (x < L.box.x || x > L.box.x + L.box.w || y < L.box.y || y > L.box.y + L.box.h) return -2;
	int i = (int)SDL_floorf((y - L.top + 3 * L.bs) / L.row);
	return i >= 0 && i < (int)m.size() && !m[i].header ? i : -1;
}
// The bottom bar's "MENU" button, for the mouse.
static SDL_FRect MenuButton(const SDL_FRect& r)
{
	float s = r.h / 450 > 1 ? r.h / 450 : 1;
	return { r.x + r.w - 62 * s, r.y + r.h - 14 * s, 62 * s, 14 * s };
}

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
	Text(r.x + 6 * s, r.y + r.h - 11 * s, s, "MONEDA, Tab o clic derecho: menu    START: volver a la posicion    Esc: salir", 200, 200, 200);
	if (P.showFrames && P.phase == Practice::Fight) DrawFrameMeter(r, P.meter);
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
	SDL_FRect mb = MenuButton(r);
	SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255);
	SDL_RenderFillRect(g_ren, &mb);
	Text(mb.x + 15 * s, mb.y + 3 * s, s, "MENU", 10, 10, 18);
	if (menu) {
		MenuLayout L = LayMenu(r, menu->size());
		float bs = L.bs, w = L.box.w;
		SDL_SetRenderDrawColor(g_ren, 10, 10, 18, 238);
		SDL_RenderFillRect(g_ren, &L.box);
		SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255);
		SDL_RenderRect(g_ren, &L.box);
		Text(L.box.x + 12 * bs, L.box.y + 8 * bs, bs, "PRACTICA", 255, 46, 136);
		for (size_t i = 0; i < menu->size(); i++) {
			const MenuItem& it = (*menu)[i];
			float y = L.top + i * L.row;
			if (it.header) {   // a group's name, small, with a line
				Text(L.box.x + 12 * bs, y + 4 * bs, bs * 0.75f, it.label.c_str(), 255, 120, 180);
				SDL_SetRenderDrawColor(g_ren, 90, 40, 70, 255);
				SDL_RenderLine(g_ren, L.box.x + 12 * bs + it.label.size() * 6 * bs + 6 * bs, y + 7 * bs, L.box.x + w - 12 * bs, y + 7 * bs);
				continue;
			}
			bool on = (int)i == sel;
			if (on) {
				SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255);
				SDL_FRect hl{ L.box.x + 6 * bs, y - 3 * bs, w - 12 * bs, L.row - 2 * bs };
				SDL_RenderFillRect(g_ren, &hl);
			}
			Uint8 c = on ? 10 : 235;
			Text(L.box.x + 16 * bs, y + 1 * bs, bs, it.label.c_str(), c, c, on ? 18 : 235);
			if (!it.value.empty()) {   // the value: green yes, grey no, yellow the rest; arrows on the selected one
				bool yes = it.value == "si", no = it.value == "no" || it.value == "no hay";
				std::string v = on ? "< " + it.value + " >" : it.value;
				Uint8 vr = on ? 10 : yes ? 120 : no ? 150 : 255, vg = on ? 10 : yes ? 255 : no ? 150 : 210, vb = on ? 18 : yes ? 160 : no ? 160 : 90;
				Text(L.box.x + w * 0.64f, y + 1 * bs, bs, v.c_str(), vr, vg, vb);
			}
			if (!it.key.empty())
				Text(L.box.x + w - (12 + 8 * it.key.size()) * bs, y + 1 * bs, bs, it.key.c_str(), on ? 60 : 130, on ? 10 : 130, on ? 40 : 150);
		}
		Text(L.box.x + 12 * bs, L.box.y + L.box.h - 13 * bs, bs * 0.75f,
		     "Flechas: elegir  Golpe/clic: cambiar  Patada/Esc: cerrar", 170, 170, 170);
	}
	SDL_RenderPresent(g_ren);
}

// The practice options are kept for next time (practica.txt), one "name value" a line.
static void PracticeOptions(Practice& P, bool save)
{
	struct { const char* name; int* i; bool* b; } opts[] = {
		{ "rival", &P.stance, NULL }, { "delay", &P.delay, NULL }, { "vida", NULL, &P.refillLife }, { "reloj", NULL, &P.freezeClock },
		{ "super", NULL, &P.fullSuper }, { "historial", NULL, &P.showHistory }, { "medidor", NULL, &P.showFrames },
	};
	std::string path = PrefFile("practica.txt");
	FILE* f = fopen(path.c_str(), save ? "w" : "r");
	if (!f) return;
	if (save) {
		for (auto& o : opts) fprintf(f, "%s %d\n", o.name, o.i ? *o.i : (int)*o.b);
	} else {
		char name[32];
		int v;
		while (fscanf(f, "%31s %d", name, &v) == 2)
			for (auto& o : opts)
				if (!strcmp(name, o.name)) { if (o.i) *o.i = v; else *o.b = v != 0; }
		if (P.stance < 0 || P.stance >= kStances) P.stance = kStand;
		if (P.delay < 0 || P.delay > 5) P.delay = 0;
	}
	fclose(f);
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
	if (shots.empty()) PracticeOptions(P, false);   // the script always starts the same
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
	auto item = [](std::string label, std::string value, std::string key, std::function<void(int)> act) {
		MenuItem m; m.label = std::move(label); m.value = std::move(value); m.key = std::move(key); m.act = std::move(act); return m;
	};
	auto buildMenu = [&]() {
		menu = {
			Header("POSICION"),
			item("Volver a la posicion", "", "START F8", [&](int) { if (P.LoadPosition()) menuOpen = false; }),
			item("Guardar esta posicion", "", "F7", [&](int) { P.SavePosition(); say("Posicion guardada"); }),
			Header("RIVAL"),
			item("Que hace", StanceName(P.stance), "F4", [&](int d) { P.stance = (P.stance + d + kStances) % kStances; P.replay = false; }),
			item(P.rec == Practice::Recording ? "Parar la grabacion" : "Grabarlo (tu lo mueves)", "", "F5", [&](int) { P.ToggleRecord(); menuOpen = false; }),
			item("Repetir la grabacion", P.tape.empty() ? "no hay" : yesNo(P.replay), "F6", [&](int) { if (!P.tape.empty()) P.replay = !P.replay; }),
			item("Moverlo tu mismo", yesNo(P.controlRival), "F3", [&](int) { if (P.rec == Practice::NoRec) P.controlRival = !P.controlRival; }),
			item("Con otro control", g_devs.size() < 2 ? "no hay" : yesNo(secondControl), "", [&](int) { secondControl = !secondControl && g_devs.size() >= 2; }),
			Header("AYUDAS"),
			item("Vida que se recupera", yesNo(P.refillLife), "", [&](int) { P.refillLife = !P.refillLife; }),
			item("Tiempo infinito", yesNo(P.freezeClock), "", [&](int) { P.freezeClock = !P.freezeClock; }),
			item("Super siempre llena", yesNo(P.fullSuper), "", [&](int) { P.fullSuper = !P.fullSuper; }),
			item("Delay como en linea", std::to_string(P.delay), "F9", [&](int d) { P.delay = (P.delay + d + 6) % 6; }),
			Header("PANTALLA"),
			item("Historial de botones", yesNo(P.showHistory), "F2", [&](int) { P.showHistory = !P.showHistory; }),
			item("Medidor de frames", yesNo(P.showFrames), "F12", [&](int) { P.showFrames = !P.showFrames; }),
			Header("SALIR"),
			item("Cambiar de personajes", "", "F10", [&](int) { P.ChangeCharacters(); menuOpen = false; }),
			item("Salir de la practica", "", "Esc", [&](int) { quit = true; }),
		};
	};
	// the next row that isn't a group's name, going up (d = -1) or down (+1)
	auto step = [&](int from, int d) {
		int n = (int)menu.size(), i = from;
		do i = (i + d + n) % n; while (menu[i].header && i != from);
		return i;
	};
	auto openMenu = [&](bool open) { menuOpen = open; buildMenu(); sel = step((int)menu.size() - 1, 1); };

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
				case SDL_SCANCODE_TAB: openMenu(!menuOpen); break;
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
				case SDL_SCANCODE_F12: P.showFrames = !P.showFrames; break;
				default: break;
				}
				if (ev.key.scancode == SDL_SCANCODE_F11 || (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)))
					SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
				break;
			// the mouse: pointing selects, left click does or changes, right click changes
			// back, the wheel moves; a click outside closes it. Right click (or MENU) opens it.
			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_WHEEL: {
				SDL_ConvertEventToRenderCoordinates(g_ren, &ev);
				SDL_FRect r = GameRect();
				if (!menuOpen) {
					SDL_FPoint pt{ ev.button.x, ev.button.y };
					SDL_FRect mb = MenuButton(r);
					if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && (ev.button.button == SDL_BUTTON_RIGHT || SDL_PointInRectFloat(&pt, &mb))) openMenu(true);
					break;
				}
				buildMenu();
				if (ev.type == SDL_EVENT_MOUSE_WHEEL) { sel = step(sel, ev.wheel.y > 0 ? -1 : 1); break; }
				bool moved = ev.type == SDL_EVENT_MOUSE_MOTION;
				int hit = MenuHit(LayMenu(r, menu.size()), menu, moved ? ev.motion.x : ev.button.x, moved ? ev.motion.y : ev.button.y);
				if (hit >= 0) sel = hit;
				if (moved) break;
				if (hit == -2) menuOpen = false;
				else if (hit >= 0) menu[hit].act(ev.button.button == SDL_BUTTON_RIGHT ? -1 : 1);
				break;
			}
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
			if (k == 240) { buildMenu(); DrawPractice(P, &menu, sel, ""); SaveWindow(shots + "/practica-menu.bmp"); quit = true; }
		}
		if (g_devs.size() < 2) secondControl = false;
		int second = secondControl ? in[1] : -1;
		uint16_t local = secondControl ? in[0] : in[0] | in[1], pressed = local & ~lastLocal;
		lastLocal = local;

		// MONEDA opens and closes the menu; START goes back to the saved position
		if (pressed & GUASA_COIN) openMenu(!menuOpen);
		if (menuOpen) {
			buildMenu();
			if (pressed & GUASA_UP) sel = step(sel, -1);
			if (pressed & GUASA_DOWN) sel = step(sel, 1);
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
	if (shots.empty()) PracticeOptions(P, true);
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
	check(ReadWord(kP2 + kLifeCopy) == kFullLife && ReadWord(kP2 + kLifeBar) == kFullLife, "las tres copias de la vida se llenan juntas");

	// a knockdown with the life refilling plays out like one without: the rival gets
	// up once (refilling only the first copy made it fall down again)
	auto knockdown = [&](bool refill) {
		P.LoadPosition();
		P.refillLife = refill;
		run(100, GUASA_RIGHT);
		run(4, GUASA_DOWN | GUASA_HK);
		std::string moves;
		for (int i = 0; i < 300; i++) { frame(0); moves += std::to_string(x2()) + "," + std::to_string(ReadWord(kP2 + 0x0A)) + " "; }
		return moves;
	};
	std::string without = knockdown(false), with = knockdown(true);
	P.refillLife = true;
	check(with == without, "derribado, el rival se levanta igual con la vida que se recupera");

	// the frame meter, against what core_test frames measured for Ryu vs Ken
	// (±1 frame: the game sometimes runs two ticks in one frame)
	auto exchange = [&](int stance, uint16_t button) {
		P.LoadPosition();
		P.stance = stance;
		run(100, GUASA_RIGHT);   // up against the rival
		run(20, 0);
		P.meter.Reset();
		run(3, button);
		int f = 0;
		while (!P.meter.done && f++ < 300) frame(0);
		const FrameMeter& m = P.meter;
		return "arranque " + std::to_string(m.startup) + ", activo " + std::to_string(m.active) + ", recuperacion " +
		       std::to_string(m.recovery) + ", ventaja " + (m.hasAdvantage ? std::to_string(m.advantage) : "-");
	};
	auto about = [](int v, int want) { return v >= want - 1 && v <= want + 1; };
	std::string d = exchange(kStand, GUASA_LP);
	const FrameMeter& M = P.meter;
	check(M.done && M.who == 0 && about(M.startup, 4) && about(M.active, 4) && about(M.recovery, 4) && M.hasAdvantage && about(M.advantage, 3),
	      "medidor: jab de pie que golpea, +3", d);
	d = exchange(kBlock, GUASA_LP);
	check(M.done && M.hasAdvantage && about(M.advantage, 4), "medidor: jab bloqueado, +4", d);
	d = exchange(kBlock, GUASA_HP);
	check(M.done && about(M.recovery, 20) && M.hasAdvantage && about(M.advantage, -7), "medidor: golpe fuerte bloqueado, -7", d);
	d = exchange(kBlockLow, GUASA_DOWN | GUASA_HK);
	check(M.done && about(M.startup, 4) && about(M.active, 5) && M.hasAdvantage && about(M.advantage, -6), "medidor: barrida bloqueada, -6", d);
	P.stance = kStand;
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
		std::string shots;
		for (int i = 5; i < argc; i++)
			if (!strncmp(argv[i], "captura=", 8)) shots = argv[i] + 8;
			else if (!ParseNetArg(argv[i], cfg)) { fprintf(stderr, "opción desconocida: %s\n", argv[i]); return 2; }
		if (argc < 6 || cfg.local < 0 || cfg.local > 1 || (cfg.remote.empty() && (cfg.relay.empty() || cfg.token.size() != 16))) {
			fprintf(stderr, "uso: guasa-game online <carpeta de ROMs> <1|2> <puerto_local> (<ip:puerto> | relay=<host:puerto> token=<32 hex> [relayonly]) [delay=N|auto] [lag=MS] [jitter=MS] [loss=PCT]\n");
			return 2;
		}
		return PlayOnline(argv[2], cfg, shots);
	}
	fprintf(stderr, "modo desconocido: %s\n", mode.c_str());
	return 2;
}
