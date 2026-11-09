// guasa-game: the game window (PLAN.md section 3). The Electron interface will
// open it for each match and talk to it over stdio; for now it also runs alone:
//
//   guasa-game check <roms>      checks the ROM folder, one line per problem
//   guasa-game versus <roms> [captura=<dir>]  two players on this PC: after each match both
//                                pick again, with a win counter (nothing is recorded)
//   guasa-game controles [archivo=<file>] [captura=<dir>]  setting up a control on a picture of the panel
//   guasa-game sonido-reto <roms> <out.wav>  the "new challenger" sound, from the player's ROM
//   guasa-game arcade <roms> [captura=<dir>]  the machine as it is, from power-on: coins, the
//                                cabinet's service menu (F2) and service credit (9); no savestates
//   guasa-game test <roms>       Test Game: versus plus both players' buttons on screen,
//                                as pressed and as the game reads them (SOCD rule)
//   guasa-game online <roms> <1|2> <local_port> (<ip:port> | relay=<host:port> token=<hex> [relayonly]) [delay=N|auto] [lag=MS] [jitter=MS] [loss=PCT]
//                                online match (rollback, net.cpp); the keyboard and the
//                                first control play this side
//   guasa-game practica <roms> [escenario=cammy|practica|juego] [captura=<dir>]
//                                practice mode (practica.h): the player against a rival
//                                that stands, blocks or repeats a recording
//   guasa-game practica-prueba <roms>   checks practice mode without a window
//   guasa-game repeticion <roms> <file> [j1=<rank|->:<name>] [j2=...]   watches a replay
//   guasa-game repeticion-prueba <roms> <file>   plays it without a window, prints the final state
//   guasa-game espectar <roms> [pausa] [j1=...] [j2=...]   watches a set live: the frames
//                                come on stdin ("cuadros <from> <base64>", "fin")
// Online, "transmitir" prints the confirmed frames for spectators ("cuadros <from> <base64>"),
// and "espectadores <n>" on stdin says how many are watching.
// Online, "grabar=<file>" saves the set's replay when the window closes.
//
// In the window: F1 sets up a control, F11 or Alt+Enter = full screen.
// Online also: F2 shows the buttons, F5 / F6 = less / more delay, F7 / F8 = mute
// my microphone / the rival (voice chat, handled by the interface), Esc leaves.
// Online, for the interface: "resultado 1|2" on stdout when a game ends, "voz mic"
// and "voz rival" for F7 and F8, "mensaje <text>" for a message to the rival (T);
// on stdin "salir" closes the window (the set is over), "voz estado mic=on|off
// rival=on|off|nada" updates the voice indicator, "mensaje <name>\t<text>"
// shows a message (":id:" is one of the league's emojis, a sticker: one every 10
// seconds) and "jugador <0|1> <rank|-> <name>" names each side, with its rank's badge.
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

// The last two are x3: one input that presses the three punches (or the three
// kicks) at once. A set where a player turns them down plays without them.
enum { kActions = 14 };
static const char* kActionNames[kActions] = { "ARRIBA", "ABAJO", "IZQUIERDA", "DERECHA", "PUNO DEBIL",
	"PUNO MEDIO", "PUNO FUERTE", "PATADA DEBIL", "PATADA MEDIA", "PATADA FUERTE", "START", "MONEDA", "X3 PUNOS", "X3 PATADAS" };
static const char* kShort[kActions] = { "Ar", "Ab", "Iz", "De", "LP", "MP", "HP", "LK", "MK", "HK", "St", "Mo", "3P", "3K" };
static bool g_noX3 = false;   // "sinx3": in this set the x3 inputs do nothing

// The game's buttons an action presses.
static uint16_t ActionBits(int a)
{
	if (a == 12) return g_noX3 ? 0 : GUASA_LP | GUASA_MP | GUASA_HP;
	if (a == 13) return g_noX3 ? 0 : GUASA_LK | GUASA_MK | GUASA_HK;
	return (uint16_t)(1 << a);
}

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

static std::string g_mapsFile;   // another file than the player's (archivo=, for checks)
static std::string MapsPath() { return g_mapsFile.empty() ? PrefFile("controles.txt") : g_mapsFile; }

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
		SDL_SCANCODE_RETURN, SDL_SCANCODE_5, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
	Mapping m{ "Teclado", {} };
	for (int i = 0; i < kActions; i++) m.b[i] = k[i] ? Bind{ 'k', (int)k[i], 1, 0 } : Bind{ 0, 0, 0, 0 };   // x3: none
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
		SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID };
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
	for (int i = 0; i < kActions; i++) if (BindDown(&d, m->second.b[i])) in |= ActionBits(i);
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
		for (int i = 0; i < kActions; i++) if (BindDown(NULL, keys.b[i])) in[0] |= ActionBits(i);
	for (size_t p = 0; p < g_devs.size() && p < 2; p++) in[p] |= ReadDevice(g_devs[p]);
}

// ---------------------------------------------------------------------------
// Window, picture and sound

static SDL_Window* g_win;
static SDL_Renderer* g_ren;
static SDL_Texture* g_tex;
static SDL_Texture* g_texSprites;   // practice's own stage: the game's picture with its black see-through
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

// ---------------------------------------------------------------------------
// guasa-game controles: setting up a control on a picture of the La Guasabara
// cabinet's panel (player 1's side, arte-app/controles), one action at a time and
// in any order: a click on a piece of it, or on the list next to it (or Enter),
// waits for the input that goes there. The same panel opens with F1 inside any
// game window (on top of the game, which gets no buttons meanwhile). Which device: the
// arrows at the top, or press anything on it. What's pressed lights up, so it
// can be tried right there. Each change is saved at once (controles.txt).

static SDL_Texture* ControlTexture(const char* name)
{
	static std::map<std::string, SDL_Texture*> cache;
	auto it = cache.find(name);
	if (it != cache.end()) return it->second;
	SDL_Texture* t = NULL;
	if (SDL_Surface* sf = SDL_LoadPNG((std::string(SDL_GetBasePath()) + "controles/" + name + ".png").c_str())) {
		t = SDL_CreateTextureFromSurface(g_ren, sf);
		SDL_DestroySurface(sf);
	}
	cache[name] = t;
	return t;
}

// What a bind is, in a few words.
static std::string BindText(const Bind& b)
{
	char s[64];
	switch (b.kind) {
	case 'k': snprintf(s, sizeof(s), "Tecla %s", SDL_GetScancodeName((SDL_Scancode)b.index)); break;
	case 'b': snprintf(s, sizeof(s), "Boton %d", b.index + 1); break;
	case 'h': snprintf(s, sizeof(s), "Cruceta %s", b.value == SDL_HAT_UP ? "arriba" : b.value == SDL_HAT_DOWN ? "abajo" :
	                   b.value == SDL_HAT_LEFT ? "izquierda" : b.value == SDL_HAT_RIGHT ? "derecha" : "?"); break;
	case 'a': snprintf(s, sizeof(s), "Eje %d %s", b.index + 1, b.value > 0 ? "+" : "-"); break;
	default: return "(sin asignar)";
	}
	return s;
}

// A gamepad without a setup works with GamepadDefault; to change it one bind at
// a time, those defaults are written as the joystick's own inputs.
static Mapping MappingFromGamepad(SDL_Gamepad* g, const std::string& name)
{
	static const SDL_GamepadButton want[kActions] = { SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
		SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
		SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_INVALID,
		SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID };
	Mapping m{ name, {} };
	int n = 0;
	SDL_GamepadBinding** binds = SDL_GetGamepadBindings(g, &n);
	for (int a = 0; a < kActions; a++)
		for (int i = 0; i < n; i++) {
			const SDL_GamepadBinding& gb = *binds[i];
			bool hit = a == 9 ? gb.output_type == SDL_GAMEPAD_BINDTYPE_AXIS && gb.output.axis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER   // heavy kick: RT
			                  : want[a] != SDL_GAMEPAD_BUTTON_INVALID && gb.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON && gb.output.button == want[a];
			if (!hit) continue;
			if (gb.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON) m.b[a] = { 'b', gb.input.button, 1, 0 };
			else if (gb.input_type == SDL_GAMEPAD_BINDTYPE_HAT) m.b[a] = { 'h', gb.input.hat.hat, gb.input.hat.hat_mask, 0 };
			else if (gb.input_type == SDL_GAMEPAD_BINDTYPE_AXIS) m.b[a] = { 'a', gb.input.axis.axis, gb.input.axis.axis_max > 0 ? 1 : -1, 0 };
			break;
		}
	SDL_free(binds);
	return m;
}


// The controls panel: state, events and drawing. Drawn at 1000 x 660 and scaled
// to the window (its own logical size while it draws or reads the mouse).
struct ControlsPanel {
	bool open = false;
	std::string devId = "teclado", devName = "Teclado";
	Mapping cur;
	int focus = 4, listen = -1, heldAxis = -1;
	bool guided = false;   // "paso a paso", like the old setup: every action in order, one after the other
	float mx = -1, my = -1;
	std::string note;
	Uint64 noteAt = 0;
	static constexpr float W = 1000, H = 660;
	// The panel (player 1's side of the La Guasabara cabinet, 515 x 340), and
	// where each piece is on it: the stick's four ways are small arrows around
	// it, the buttons and start are circles (found on the picture)
	static constexpr float panelW = 515, panelH = 340, k = 470 / panelH, px0 = 24, py0 = 120;
	struct Spot { SDL_FPoint c; float r; bool round; float h = 0; };   // h: a box's half height, if not square
	Spot spots[kActions];
	float listX = 0, listW = 0;
	static constexpr float rowH = 470.0f / kActions, listY = 120;
	SDL_FRect prev{ 152, 66, 34, 30 }, next{ 560, 66, 34, 30 }, steps{ 464, 606, 160, 38 }, reset{ 640, 606, 160, 38 }, done{ 816, 606, 160, 38 };

	ControlsPanel()
	{
		auto at = [](float x, float y) { return SDL_FPoint{ px0 + x * k, py0 + y * k }; };
		const float stickX = 118, stickY = 188, arm = 60, arrow = 15;
		Spot all[kActions] = {
			{ at(stickX, stickY - arm), arrow * k, false }, { at(stickX, stickY + arm), arrow * k, false },
			{ at(stickX - arm, stickY), arrow * k, false }, { at(stickX + arm, stickY), arrow * k, false },
			{ at(227, 166), 41 * k, true }, { at(317, 164), 41 * k, true }, { at(410, 166), 41 * k, true },
			{ at(224, 262), 41 * k, true }, { at(316, 255), 41 * k, true }, { at(409, 257), 41 * k, true },
			{ at(428, 44), 39 * k, true },
			{ at(471, 311), 43 * k, false, 28 * k },   // coin: the panel has none, so a box in its free corner
			{ at(58, 26), 40 * k, false, 14 * k },     // x3: two boxes in the empty top left
			{ at(58, 62), 40 * k, false, 14 * k },
		};
		for (int a = 0; a < kActions; a++) spots[a] = all[a];
		listX = px0 + panelW * k + 16;
		listW = W - 24 - listX;
	}
	Device* device() { for (auto& d : g_devs) if (d.id == devId) return &d; return NULL; }
	void pick(const std::string& id)
	{
		devId = id;
		Device* d = device();
		devName = d ? d->name : "Teclado";
		auto m = g_maps.find(id);
		cur = m != g_maps.end() ? m->second : id == "teclado" ? KeyboardDefault() : d && d->pad ? MappingFromGamepad(d->pad, devName) : Mapping{ devName, {} };
	}
	void Open() { pick(g_devs.empty() ? "teclado" : g_devs[0].id); listen = -1; guided = false; open = true; }
	// after an action (assigned or skipped): in "paso a paso", on to the next one
	void advance(int was)
	{
		listen = -1;
		if (!guided) return;
		if (was + 1 < kActions) { listen = focus = was + 1; heldAxis = -1; }
		else { guided = false; say("Listo: " + devName + " configurado"); }
	}
	void save() { cur.name = devName; g_maps[devId] = cur; SaveMaps(); printf("control configurado: %s\n", devName.c_str()); fflush(stdout); }
	void say(const std::string& t) { note = t; noteAt = SDL_GetTicksNS(); }
	SDL_FRect row(int a) const { return SDL_FRect{ listX, listY + a * rowH + 3, listW, rowH - 3 }; }
	static bool inside(const SDL_FRect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
	int spotAt(float x, float y) const
	{
		for (int a = 0; a < kActions; a++) {
			if (inside(row(a), x, y)) return a;
			const Spot& sp = spots[a];
			float dx = x - sp.c.x, dy = y - sp.c.y;
			if (sp.round ? dx * dx + dy * dy <= sp.r * sp.r : SDL_fabsf(dx) <= sp.r && SDL_fabsf(dy) <= (sp.h ? sp.h : sp.r)) return a;
		}
		return -1;
	}
	void assign(const Bind& b)
	{
		for (int j = 0; j < kActions; j++)   // the same input somewhere else: it moves here
			if (j != listen && cur.b[j].kind == b.kind && cur.b[j].index == b.index && cur.b[j].value == b.value) cur.b[j] = Bind{ 0, 0, 0, 0 };
		cur.b[listen] = b;
		save();
		say(std::string(kActionNames[listen]) + ": " + BindText(b));
		advance(listen);
	}
	void clear(int a)   // an action left empty
	{
		cur.b[a] = Bind{ 0, 0, 0, 0 };
		save();
		say(std::string(kActionNames[a]) + ": sin asignar");
	}
	void step(int d)
	{
		std::vector<std::string> ids{ "teclado" };
		for (auto& dv : g_devs) ids.push_back(dv.id);
		size_t i = 0;
		while (i < ids.size() && ids[i] != devId) i++;
		pick(ids[(i + ids.size() + d) % ids.size()]);
		listen = -1;
		guided = false;
	}

	// While open it takes every event but quitting and controls coming and going
	// (the caller still handles those). False when closed.
	bool Event(const SDL_Event& in)
	{
		if (!open) return false;
		if (in.type == SDL_EVENT_QUIT || in.type == SDL_EVENT_JOYSTICK_ADDED) return false;
		if (in.type == SDL_EVENT_JOYSTICK_REMOVED) return false;
		SDL_Event ev = in;
		int lw, lh;
		SDL_RendererLogicalPresentation mode;
		SDL_GetRenderLogicalPresentation(g_ren, &lw, &lh, &mode);
		SDL_SetRenderLogicalPresentation(g_ren, (int)W, (int)H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		SDL_ConvertEventToRenderCoordinates(g_ren, &ev);
		SDL_SetRenderLogicalPresentation(g_ren, lw, lh, mode);
		if (ev.type == SDL_EVENT_MOUSE_MOTION) { mx = ev.motion.x; my = ev.motion.y; }
		// which device sent it (empty: none of the joysticks)
		std::string from;
		SDL_JoystickID which = ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? ev.jbutton.which : ev.type == SDL_EVENT_JOYSTICK_HAT_MOTION ? ev.jhat.which
		                     : ev.type == SDL_EVENT_JOYSTICK_AXIS_MOTION ? ev.jaxis.which : 0;
		if (which) for (auto& d : g_devs) if (SDL_GetJoystickID(d.joy) == which) from = d.id;
		if (listen >= 0) {
			if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && devId != "teclado" &&
			    (ev.key.scancode == SDL_SCANCODE_DELETE || ev.key.scancode == SDL_SCANCODE_BACKSPACE)) {   // empty (on the keyboard, Supr is a key to assign)
				clear(listen);
				advance(listen);
				return true;
			}
			if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_ESCAPE) {
				if (guided) advance(listen);   // paso a paso: Esc leaves this one as it is and goes on
				else { listen = -1; say("Cancelado"); }
				return true;
			}
			if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
				listen = -1;
				guided = false;
				if (ev.button.button == SDL_BUTTON_RIGHT) { int a = spotAt(ev.button.x, ev.button.y); if (a >= 0) clear(a); }
				return true;
			}
			if (devId == "teclado") {
				if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) assign(Bind{ 'k', (int)ev.key.scancode, 1, 0 });
				return true;
			}
			if (from != devId) return true;
			Device* d = device();
			if (ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) assign(Bind{ 'b', ev.jbutton.button, 1, 0 });
			else if (ev.type == SDL_EVENT_JOYSTICK_HAT_MOTION && ev.jhat.value) {
				// one direction even if a diagonal comes first; for a direction, the one asked for
				Uint8 v = ev.jhat.value, want = listen == 0 ? SDL_HAT_UP : listen == 1 ? SDL_HAT_DOWN : listen == 2 ? SDL_HAT_LEFT : listen == 3 ? SDL_HAT_RIGHT : 0;
				if (want && !(v & want)) return true;
				if (!want) want = (v & SDL_HAT_UP) ? SDL_HAT_UP : (v & SDL_HAT_DOWN) ? SDL_HAT_DOWN : (v & SDL_HAT_LEFT) ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
				assign(Bind{ 'h', ev.jhat.hat, want, 0 });
			} else if (ev.type == SDL_EVENT_JOYSTICK_AXIS_MOTION && d) {
				int rest = ev.jaxis.axis < (int)d->rest.size() ? d->rest[ev.jaxis.axis] : 0, v = ev.jaxis.value - rest;
				if (ev.jaxis.axis == heldAxis) { if (abs(v) < 8000) heldAxis = -1; return true; }
				if (abs(v) > 16000) { heldAxis = ev.jaxis.axis; assign(Bind{ 'a', ev.jaxis.axis, v > 0 ? 1 : -1, rest }); }
			}
			return true;
		}
		// not waiting for an input: a press on another control picks that control
		if (!from.empty() && from != devId && (ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || (ev.type == SDL_EVENT_JOYSTICK_HAT_MOTION && ev.jhat.value))) {
			pick(from);
			say("Configurando: " + devName);
		}
		if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
			switch (ev.key.scancode) {
			case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F1: open = false; break;
			case SDL_SCANCODE_F2: guided = true; listen = focus = 0; heldAxis = -1; break;
			case SDL_SCANCODE_TAB: case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_DOWN: focus = (focus + 1) % kActions; break;
			case SDL_SCANCODE_LEFT: case SDL_SCANCODE_UP: focus = (focus + kActions - 1) % kActions; break;
			case SDL_SCANCODE_RETURN: case SDL_SCANCODE_SPACE: listen = focus; heldAxis = -1; break;
			case SDL_SCANCODE_DELETE: case SDL_SCANCODE_BACKSPACE: clear(focus); break;
			case SDL_SCANCODE_PAGEUP: step(-1); break;
			case SDL_SCANCODE_PAGEDOWN: step(1); break;
			default: break;
			}
		}
		if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT) {   // right click: leave it empty
			int a = spotAt(ev.button.x, ev.button.y);
			if (a >= 0) { focus = a; clear(a); }
		}
		if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
			float x = ev.button.x, y = ev.button.y;
			int a = spotAt(x, y);
			if (a >= 0) { focus = listen = a; heldAxis = -1; }
			else if (inside(prev, x, y)) step(-1);
			else if (inside(next, x, y)) step(1);
			else if (inside(done, x, y)) open = false;
			else if (inside(steps, x, y)) { guided = true; listen = focus = 0; heldAxis = -1; }
			else if (inside(reset, x, y)) {
				g_maps.erase(devId);
				SaveMaps();
				pick(devId);
				say("Como venia de fabrica");
			}
		}
		return true;
	}

	void Draw()
	{
		if (!open) return;
		if (devId != "teclado" && !device()) pick("teclado");   // its control went away
		int lw, lh;
		SDL_RendererLogicalPresentation mode;
		SDL_GetRenderLogicalPresentation(g_ren, &lw, &lh, &mode);
		SDL_SetRenderLogicalPresentation(g_ren, (int)W, (int)H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		// what's pressed now on the device being set up
		Device* d = device();
		bool down[kActions];
		for (int a = 0; a < kActions; a++) down[a] = (devId == "teclado" || d) && BindDown(d, cur.b[a]);
		bool blink = (SDL_GetTicks() / 250) % 2;
		int hover = spotAt(mx, my);
		const char* color[6] = { "rojo", "blanco", "azul", "rojo", "blanco", "azul" };
		const char* arrows[4] = { "^", "v", "<", ">" };

		SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(g_ren, 13, 14, 19, 240);   // over the game, if there is one
		SDL_FRect all{ 0, 0, W, H };
		SDL_RenderFillRect(g_ren, &all);
		Text(24, 20, 3, "CONTROLES", 255, 46, 136);
		Text(300, 28, 1.5f, "Haz clic en una pieza y aprieta lo que va ahi", 200, 200, 210);
		// the device, with arrows to change it
		Text(24, 74, 1.75f, "Control:", 200, 200, 210);
		for (const SDL_FRect* r : { &prev, &next }) {
			SDL_SetRenderDrawColor(g_ren, 40, 42, 54, 255);
			SDL_RenderFillRect(g_ren, r);
			Text(r->x + 11, r->y + 8, 1.75f, r == &prev ? "<" : ">", 255, 255, 255);
		}
		std::string shown = devName.substr(0, 40);
		Text(prev.x + 46, 74, 1.75f, shown.c_str(), 255, 220, 120);
		Text(next.x + 48, 76, 1.25f, "o aprieta algo en ese control", 150, 150, 160);
		// the panel, and on it what's pressed and what's chosen
		if (SDL_Texture* t = ControlTexture("panel")) { SDL_FRect r{ px0, py0, panelW * k, panelH * k }; SDL_RenderTexture(g_ren, t, NULL, &r); }
		for (int a = 0; a < kActions; a++) {
			const Spot& sp = spots[a];
			bool wait = a == listen, on = a == focus || a == hover;
			float hh = sp.h ? sp.h : sp.r;
			SDL_FRect r{ sp.c.x - sp.r, sp.c.y - hh, 2 * sp.r, 2 * hh };
			if (sp.round) {
				if (down[a]) {   // pressed: the button pressed, on top of the panel's
					std::string name = std::string("boton-") + (a < 10 ? color[a - 4] : "blanco") + "-apretado";
					if (SDL_Texture* t = ControlTexture(name.c_str())) SDL_RenderTexture(g_ren, t, NULL, &r);
				}
			} else {   // a way of the stick (an arrow), the coin or x3: yellow while pressed
				SDL_SetRenderDrawColor(g_ren, down[a] ? 255 : 0, down[a] ? 220 : 0, down[a] ? 60 : 0, down[a] ? 255 : a >= 11 ? 235 : 170);
				SDL_RenderFillRect(g_ren, &r);
				Uint8 c = down[a] ? 20 : 255;
				if (a < 4) Text(sp.c.x - 8, sp.c.y - 8, 2, arrows[a], c, c, c);
				else {
					SDL_SetRenderDrawColor(g_ren, 212, 160, 60, 255);   // a golden edge, like the panel's labels
					SDL_RenderRect(g_ren, &r);
					const char* label = a == 11 ? "MONEDA" : a == 12 ? "X3 PUNOS" : "X3 PATADAS";
					float ts = a == 11 ? 1.5f : 1.1f;
					Text(sp.c.x - strlen(label) * 4 * ts, sp.c.y - 4 * ts, ts, label, down[a] ? 20 : 255, down[a] ? 20 : 220, down[a] ? 20 : 120);
				}
			}
			if (wait || on) {   // chosen: a ring (pink and blinking while it waits for the input)
				if (wait && !blink) continue;
				SDL_SetRenderDrawColor(g_ren, 255, wait ? 46 : 220, wait ? 136 : 120, 255);
				for (int t = 0; t < (wait ? 4 : 2); t++) { SDL_FRect o{ r.x - 4 - t, r.y - 4 - t, r.w + 8 + 2 * t, r.h + 8 + 2 * t }; SDL_RenderRect(g_ren, &o); }
			}
		}
		// the list: every action, what it has, lit while pressed
		for (int a = 0; a < kActions; a++) {
			SDL_FRect r = row(a);
			bool wait = a == listen, on = a == focus || a == hover;
			SDL_SetRenderDrawColor(g_ren, down[a] ? 255 : 28, down[a] ? 220 : 30, down[a] ? 120 : 40, 255);
			SDL_RenderFillRect(g_ren, &r);
			if (wait ? blink : on) {
				SDL_SetRenderDrawColor(g_ren, 255, wait ? 46 : 220, wait ? 136 : 120, 255);
				SDL_RenderRect(g_ren, &r);
			}
			Uint8 c = down[a] ? 20 : 255;
			Text(r.x + 8, r.y + 3, 1.25f, kActionNames[a], c, c, c);
			std::string bind = wait ? "Aprieta..." : BindText(cur.b[a]);
			Text(r.x + 8, r.y + 16, 1, bind.c_str(), down[a] ? 40 : wait ? 255 : 170, down[a] ? 40 : wait ? 120 : 190, down[a] ? 40 : wait ? 170 : 220);
		}
		// the bottom: what's happening, and the buttons
		std::string line = listen >= 0 && guided ? std::string("PASO A PASO ") + std::to_string(listen + 1) + "/" + std::to_string(kActions) + ": aprieta " + kActionNames[listen] + " (Esc: saltar)"
		                 : listen >= 0 ? std::string("Aprieta lo que va en ") + kActionNames[listen] + (devId == "teclado" ? "  (Esc: cancelar)" : "  (Supr: vacio, Esc: cancelar)")
		                 : note.size() && SDL_GetTicksNS() - noteAt < 4000000000ULL ? note.substr(0, 52)
		                 : "Clic: asignar   Clic derecho o Supr: vaciar   Esc: cerrar";
		Text(24, 620, 1, line.c_str(), listen >= 0 ? 255 : 200, listen >= 0 ? 120 : 200, listen >= 0 ? 170 : 210);
		for (const SDL_FRect* r : { &steps, &reset, &done }) {
			bool h = inside(*r, mx, my) || (r == &steps && guided);
			SDL_SetRenderDrawColor(g_ren, r == &done ? 255 : 40, r == &done ? 46 : 42, r == &done ? 136 : 54, h ? 255 : 220);
			SDL_RenderFillRect(g_ren, r);
			const char* t = r == &done ? "LISTO" : r == &reset ? "DE FABRICA" : "PASO A PASO";
			Text(r->x + (r->w - strlen(t) * 12) / 2, r->y + 12, 1.5f, t, 255, 255, 255);
		}
		SDL_SetRenderLogicalPresentation(g_ren, lw, lh, mode);
	}
};
static ControlsPanel g_ctl;   // F1 in any game window

static std::function<void(const SDL_FRect&)> g_overlay;   // drawn last (messages in online sets)

// show = false: drawn but not shown yet (a picture of it is taken first)
static void Present(const uint16_t* raw, const char* banner, const NetStats* stats = NULL, int local = 0, bool show = true)
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
	g_ctl.Draw();
	if (show) SDL_RenderPresent(g_ren);
}


// ---------------------------------------------------------------------------

static void CloseAll();

static void SaveWindow(const std::string& path);

// Random presses like the bot's, for checking Versus without hands.
static uint16_t RandomPad(int frame, uint64_t& rng)
{
	uint16_t in = 0;
	for (int k = 0; k < 10; k++) {
		rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
		if ((uint32_t)(rng >> 33) % 100 < 30) in |= 1 << k;
	}
	if (frame % 1200 < 6) in |= GUASA_START;
	return in;
}

// shots: "captura=<folder>" (checking the screen without hands): both sides press
// at random, as fast as it goes; a picture when the first match is won, when both
// are back at character select and when the second is won; then it closes.
static int Play(const char* roms, bool test, const std::string& shots = "")
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

	// Versus: after each match both go back to character select (not only the
	// loser, as the machine does), with a counter of matches won.
	std::vector<uint8_t> select(guasa_state_size());
	guasa_save(select.data(), select.size());
	ResultWatcher results;
	int wins[2] = { 0, 0 }, lastWinner = 0;
	int backIn = 0;   // frames left to go back to character select (4 s after a match)
	int frame = 0, shotAt = -1;
	std::string shotName;
	uint64_t rng[2] = { 1, 2 };

	const Uint64 frameNs = (Uint64)(1e9 / guasa_fps());   // 59.63 frames per second
	Uint64 next = SDL_GetTicksNS();
	bool quit = false;
	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (g_ctl.Event(ev)) continue;
			switch (ev.type) {
			case SDL_EVENT_QUIT: quit = true; break;
			case SDL_EVENT_JOYSTICK_ADDED: AddDevice(ev.jdevice.which); break;
			case SDL_EVENT_JOYSTICK_REMOVED: RemoveDevice(ev.jdevice.which); break;
			case SDL_EVENT_KEY_DOWN:
				if (ev.key.scancode == SDL_SCANCODE_F1) g_ctl.Open();
				if (ev.key.scancode == SDL_SCANCODE_ESCAPE && !test) quit = true;
				if (ev.key.scancode == SDL_SCANCODE_F11 || (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)))
					SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
				break;
			}
		}
		uint16_t in[2] = { 0, 0 };
		if (!shots.empty()) { in[0] = RandomPad(frame, rng[0]); in[1] = RandomPad(frame, rng[1]); }
		else if (!g_ctl.open) ReadPlayers(in);   // while setting up a control the game gets nothing
		guasa_run_frame(in, 1);
		frame++;
		if (shots.empty()) PushAudio();
		if (!test) {
			if (int w = results.Update()) {
				wins[w - 1]++;
				lastWinner = w;
				backIn = 240;
				printf("gana el jugador %d (%d - %d)\n", w, wins[0], wins[1]);
				fflush(stdout);
				shotAt = frame + 60, shotName = wins[0] + wins[1] == 1 ? "versus-gana" : "versus-segunda";
			}
			if (backIn && --backIn == 0) {   // both pick again
				guasa_load(select.data(), select.size());
				results = ResultWatcher();
				printf("los dos eligen\n");
				fflush(stdout);
				if (wins[0] + wins[1] == 1) shotAt = frame + 90, shotName = "versus-eligen";
			}
			g_overlay = [&](const SDL_FRect& r) {
				// the counter at the top middle, in the gap between the score line and the
				// K.O. of the life bars: nothing of the game there
				float s = r.h / 450 > 1 ? r.h / 450 : 1;
				char line[80];
				snprintf(line, sizeof(line), "J1  %d - %d  J2", wins[0], wins[1]);
				float w = (float)strlen(line) * 8 * s + 16 * s;
				SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
				SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 160);
				SDL_FRect bar{ r.x + r.w / 2 - w / 2, r.y + 29 * s, w, 12 * s };
				SDL_RenderFillRect(g_ren, &bar);
				Text(bar.x + 8 * s, bar.y + 2 * s, s, line, 235, 235, 235);
				if (frame < 600) Text(r.x + 6 * s, r.y + r.h - 13 * s, s, "Esc: salir", 200, 200, 200);
				if (backIn) {   // who won, until both go back to pick
					char who[48];
					snprintf(who, sizeof(who), "GANA EL JUGADOR %d", lastWinner);
					SDL_FRect b{ r.x + r.w / 2 - 110 * s, r.y + r.h * 0.3f, 220 * s, 22 * s };
					SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 200);
					SDL_RenderFillRect(g_ren, &b);
					Text(b.x + 34 * s, b.y + 7 * s, s, who, 255, 220, 120);
				}
			};
		}
		if (!shots.empty()) {   // as fast as it goes: drawn only for the pictures
			if (frame == shotAt) {
				Present(NULL, NULL, NULL, 0, false);
				SaveWindow(shots + "/" + shotName + ".bmp");
				SDL_RenderPresent(g_ren);
				if (shotName == "versus-segunda") quit = true;
			}
			if (frame > 120000) { printf("no termino ninguna pelea\n"); quit = true; }
			continue;
		}
		std::string banner = "";
		Present(test ? in : NULL, banner.empty() ? NULL : banner.c_str());

		next += frameNs;
		Uint64 now = SDL_GetTicksNS();
		if (next > now) SDL_DelayPrecise(next - now);
		else if (now - next > 5 * frameNs) next = now;   // far behind (window dragged, PC busy): don't rush to catch up
	}
	g_overlay = nullptr;
	CloseAll();
	return 0;
}

// Arcade: the machine as it is, from power-on (its warning, the attract, coins
// and start), with the cabinet's buttons on the keyboard: F2 its service menu,
// 9 a service credit. Two players like at the arcade: coins, the winner stays.
// No savestates, no counter, nothing recorded. The service menu's settings are
// kept like a real cabinet's (its EEPROM, arcade.nv), never a savestate.
// shots: "captura=<folder>": the power-on with the keys, a coin and start, the
// service menu (F2 held), out of it; a picture of each, then it closes.
static int PlayArcade(const char* roms, const std::string& shots = "")
{
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) {
		char report[4000] = "";
		guasa_check_rom(roms, report, sizeof(report));
		printf("error: %s\n%s", msg, report);
		return 1;
	}
	std::string nvPath = PrefFile("arcade.nv");
	if (FILE* f = fopen(nvPath.c_str(), "rb")) {   // the cabinet's settings from last time
		std::vector<uint8_t> nv(65536);
		size_t n = fread(nv.data(), 1, nv.size(), f);
		fclose(f);
		if (guasa_nvram_load(nv.data(), n)) printf("ajustes de la maquina: no sirven, quedan los de fabrica\n");
	}
	if (!OpenWindow("La Guasabara - Arcade")) return 1;
	LoadMaps();
	printf("arcade\n");
	fflush(stdout);

	const Uint64 frameNs = (Uint64)(1e9 / guasa_fps());
	Uint64 next = SDL_GetTicksNS(), start = next;
	int frame = 0;
	bool quit = false;
	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (g_ctl.Event(ev)) continue;
			switch (ev.type) {
			case SDL_EVENT_QUIT: quit = true; break;
			case SDL_EVENT_JOYSTICK_ADDED: AddDevice(ev.jdevice.which); break;
			case SDL_EVENT_JOYSTICK_REMOVED: RemoveDevice(ev.jdevice.which); break;
			case SDL_EVENT_KEY_DOWN:
				if (ev.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;
				if (ev.key.scancode == SDL_SCANCODE_F1) g_ctl.Open();
				if (ev.key.scancode == SDL_SCANCODE_F11 || (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)))
					SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
				break;
			}
		}
		uint16_t in[2] = { 0, 0 };
		const bool* keys = SDL_GetKeyboardState(NULL);
		bool diag = !g_ctl.open && keys[SDL_SCANCODE_F2], service = !g_ctl.open && keys[SDL_SCANCODE_9];
		if (!shots.empty()) {   // the script: a coin, start, then the service menu
			in[0] = (frame >= 1500 && frame < 1506 ? GUASA_COIN : 0) | (frame >= 1600 && frame < 1606 ? GUASA_START : 0);
			diag = frame >= 2200 && frame < 2210;
		} else if (!g_ctl.open) ReadPlayers(in);
		guasa_cabinet(diag, service);
		guasa_run_frame(in, 1);
		frame++;
		PushAudio();
		bool hint = shots.empty() ? SDL_GetTicksNS() - start < 12000000000ULL : frame < 1000;   // the keys, for the first seconds
		g_overlay = [&](const SDL_FRect& r) {
			if (!hint) return;
			float s = r.h / 450 > 1 ? r.h / 450 : 1;
			SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 170);
			SDL_FRect bar{ r.x, r.y + r.h - 14 * s, r.w, 14 * s };
			SDL_RenderFillRect(g_ren, &bar);
			Text(r.x + 6 * s, r.y + r.h - 11 * s, s, "MONEDA y START   F2 menu de servicio   9 credito   Esc salir", 220, 220, 220);
		};
		std::string banner = "";
		Present(NULL, banner.empty() ? NULL : banner.c_str(), NULL, 0, shots.empty());
		if (!shots.empty()) {
			for (auto [f, name] : { std::pair<int, const char*>{ 400, "arcade-encendido" }, { 1560, "arcade-moneda" },
			                        { 1900, "arcade-start" }, { 1960, "arcade-f1" }, { 2600, "arcade-servicio" } })
				if (frame == f) SaveWindow(shots + "/" + name + ".bmp");
			if (frame == 1950) g_ctl.Open();   // F1: the controls panel over the game
			if (frame == 1961) g_ctl.open = false;
			SDL_RenderPresent(g_ren);
			if (frame == 2600) quit = true;
			continue;
		}

		next += frameNs;
		Uint64 now = SDL_GetTicksNS();
		if (next > now) SDL_DelayPrecise(next - now);
		else if (now - next > 5 * frameNs) next = now;
	}
	std::vector<uint8_t> nv(65536);   // the cabinet's settings, for next time
	if (size_t n = guasa_nvram_save(nv.data(), nv.size()))
		if (FILE* f = fopen(nvPath.c_str(), "wb")) { fwrite(nv.data(), 1, n, f); fclose(f); }
	g_overlay = nullptr;
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
static std::atomic<int> g_watchers{ 0 };   // spectators of this set ("espectadores <n>")

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

// Each side's player, as the app says: name and rank (empty: no rank yet), the
// PNG of their profile photo (empty: none), and the set's score (-1: not known yet).
static std::mutex g_namesLock;
static std::string g_names[2], g_ranks[2], g_photos[2];
static std::atomic<int> g_score[2] = { -1, -1 };

// A profile photo the app left in a file, loaded once (the next frame after it arrives).
static SDL_Texture* PhotoTexture(const std::string& file)
{
	static std::map<std::string, SDL_Texture*> cache;
	if (file.empty()) return NULL;
	auto it = cache.find(file);
	if (it != cache.end()) return it->second;
	SDL_Texture* t = NULL;
	if (SDL_Surface* sf = SDL_LoadPNG(file.c_str())) {
		t = SDL_CreateTextureFromSurface(g_ren, sf);
		SDL_DestroySurface(sf);
	}
	cache[file] = t;
	return t;
}

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

// The letters of that bar: Roboto Bold, drawn once in a sheet (tools/preparar_fuente.py,
// controles/fuente.png and fuente.txt) and put together here, smooth at any size.
struct Glyph { float x, y, w, h, xo, yo, adv; };
static struct SheetFont {
	bool tried = false, ok = false;
	float size = 48, line = 57;
	Glyph g[128] = {};
	void Load()
	{
		if (tried) return;
		tried = true;
		std::string base = std::string(SDL_GetBasePath()) + "controles/";
		FILE* f = fopen((base + "fuente.txt").c_str(), "r");
		if (!f) return;
		char buf[200];
		while (fgets(buf, sizeof(buf), f)) {
			int c;
			Glyph x;
			float asc;
			if (sscanf(buf, "linea %f %f", &line, &asc) == 2) continue;
			if (sscanf(buf, "%d %f %f %f %f %f %f %f", &c, &x.x, &x.y, &x.w, &x.h, &x.xo, &x.yo, &x.adv) == 8 && c >= 32 && c < 128) g[c] = x;
		}
		fclose(f);
		ok = ControlTexture("fuente") != NULL;
	}
	// width of text at px tall letters (the sheet's size)
	float Width(const std::string& t, float px)
	{
		float w = 0;
		for (unsigned char c : t) if (c >= 32 && c < 128) w += g[c].adv;
		return w * px / size;
	}
	// text with the top of its line at y; px: the font's size on screen
	void Draw(float x, float y, float px, const std::string& t, Uint8 r, Uint8 gg, Uint8 b)
	{
		SDL_Texture* tex = ControlTexture("fuente");
		if (!tex) return;
		float k = px / size;
		SDL_SetTextureColorMod(tex, r, gg, b);
		for (unsigned char c : t) {
			if (c < 32 || c >= 128) continue;
			const Glyph& q = g[c];
			if (q.w > 0) {
				SDL_FRect src{ q.x, q.y, q.w, q.h }, dst{ x + q.xo * k, y + q.yo * k, q.w * k, q.h * k };
				SDL_RenderTexture(g_ren, tex, &src, &dst);
			}
			x += q.adv * k;
		}
		SDL_SetTextureColorMod(tex, 255, 255, 255);
	}
} g_font;

// Under the top line, in the middle, like Fightcade's: "photo name 2 VS 1 name
// photo" (player 1 on the left), the score in the set's color. Without a photo,
// the rank's badge.
static void DrawNames(const SDL_FRect& r)
{
	std::string names[2], ranks[2], photos[2];
	{
		std::lock_guard<std::mutex> lock(g_namesLock);
		for (int p = 0; p < 2; p++) { names[p] = Plain(g_names[p]); ranks[p] = g_ranks[p]; photos[p] = g_photos[p]; }
	}
	if (names[0].empty() && names[1].empty()) return;
	g_font.Load();
	float s = r.h / 450 > 1 ? r.h / 450 : 1, h = 26 * s, pic = h - 4 * s, gap = 8 * s;
	float px = 21 * s;   // the letters' size
	char sc[2][8];
	for (int p = 0; p < 2; p++) snprintf(sc[p], sizeof(sc[p]), "%d", SDL_max((int)g_score[p], 0));
	auto width = [&](const char* t) { return g_font.ok ? g_font.Width(t, px) : strlen(t) * 8 * 1.5f * s; };
	// the VS in the middle of the game, over the K.O., always; each side grows away
	// from it, and the bar is as wide as the longer side on both
	float mid = r.x + r.w / 2, vs = width("VS");
	float side[2];
	for (int p = 0; p < 2; p++) side[p] = pic + gap + width(names[p].c_str()) + gap + width(sc[p]) + gap;
	float half = vs / 2 + SDL_max(side[0], side[1]) + gap;
	SDL_FRect box{ mid - half, r.y + 15 * s, 2 * half, h };
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 8, 10, 16, 200);
	SDL_RenderFillRect(g_ren, &box);
	float x = mid - vs / 2 - side[0], ty = box.y + (h - (g_font.ok ? g_font.line * px / g_font.size : 12 * s)) / 2;
	auto face = [&](int p) {
		SDL_FRect f{ x, box.y + 2 * s, pic, pic };
		SDL_Texture* t = PhotoTexture(photos[p]);
		if (!t && !ranks[p].empty()) t = RankTexture(ranks[p]);
		if (t) SDL_RenderTexture(g_ren, t, NULL, &f);
		x += pic + gap;
	};
	auto word = [&](const char* text, Uint8 cr, Uint8 cg, Uint8 cb) {
		if (g_font.ok) g_font.Draw(x, ty, px, text, cr, cg, cb);
		else Text(x, ty, 1.5f * s, text, cr, cg, cb);   // without the sheet, the plain letters
		x += width(text) + gap;
	};
	Uint8 nr, ng, nb;
	face(0);
	NameColor(g_names[0], nr, ng, nb);
	word(names[0].c_str(), nr, ng, nb);
	word(sc[0], 80, 210, 230);
	x = mid - vs / 2;
	word("VS", 150, 150, 160);
	x = mid + vs / 2 + gap;
	word(sc[1], 80, 210, 230);
	NameColor(g_names[1], nr, ng, nb);
	word(names[1].c_str(), nr, ng, nb);
	face(1);
}

// At the bottom, in the middle: an eye and how many are watching the set (only
// while someone is), so the players know.
static void DrawWatchers(const SDL_FRect& r)
{
	int n = g_watchers;
	if (n <= 0) return;
	float s = r.h / 450 > 1 ? r.h / 450 : 1, h = 18 * s, eye = 20 * s;
	char num[8];
	snprintf(num, sizeof(num), "%d", n);
	float w = eye + 10 * s + strlen(num) * 8 * 1.5f * s + 14 * s;
	SDL_FRect box{ r.x + r.w / 2 - w / 2, r.y + r.h - h - 6 * s, w, h };
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 8, 10, 16, 190);
	SDL_RenderFillRect(g_ren, &box);
	if (SDL_Texture* t = ControlTexture("ojo")) { SDL_FRect e{ box.x + 7 * s, box.y + (h - eye * 0.6f) / 2, eye, eye * 0.6f }; SDL_RenderTexture(g_ren, t, NULL, &e); }
	Text(box.x + eye + 14 * s, box.y + (h - 12 * s) / 2, 1.5f * s, num, 235, 235, 240);
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
		Text(L.box.x + 10 * s, L.box.y + L.box.h - 18 * s, s, "Un sticker cada 10 segundos", 170, 170, 170);
	}
};

static void SaveWindow(const std::string& path);

// shots: "captura=<folder>" (checking the screen without hands): once playing, a
// message arrives, then the panel opens, then the pause; a picture of each.
static int PlayOnline(const char* roms, const NetConfig& cfg, const std::string& shots = "", const std::string& recordTo = "", bool stream = false)
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
			if (line.rfind("mensaje ", 0) == 0) {   // "mensaje <name>\t<text>"
				size_t tab = line.find('\t', 8);
				std::lock_guard<std::mutex> lock(inboxLock);
				if (tab != std::string::npos) inbox.push_back({ line.substr(8, tab - 8), line.substr(tab + 1), 0, 0 });
			}
			if (line.rfind("espectadores ", 0) == 0) g_watchers = atoi(line.c_str() + 13);
			if (line.rfind("marcador ", 0) == 0) {   // "marcador <p1> <p2>": the set's score
				int a = 0, b = 0;
				if (sscanf(line.c_str() + 9, "%d %d", &a, &b) == 2) { g_score[0] = a; g_score[1] = b; }
			}
			if (line.rfind("foto ", 0) == 0 && line.size() > 7 && (line[5] == '0' || line[5] == '1')) {   // "foto <0|1> <png>"
				std::lock_guard<std::mutex> lock(g_namesLock);
				g_photos[line[5] - '0'] = line.substr(7);
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
	size_t streamed = 0;         // frames already sent to the spectators
	// the spectators' frames: those no rollback can change anymore (GekkoNet
	// predicts at most 8 ahead), every half second; all of them at the end
	auto streamFrames = [&](bool all) {
		const std::vector<uint32_t>& rec = net.Recorded();
		size_t upto = all ? rec.size() : rec.size() > 12 ? rec.size() - 12 : 0;
		if (!stream || upto <= streamed || (!all && upto - streamed < 30)) return;
		printf("cuadros %zu %s\n", streamed, FramesToBase64(rec.data() + streamed, upto - streamed).c_str());
		fflush(stdout);
		streamed = upto;
	};
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
	Uint64 stickerAt = 0;   // one sticker every 10 seconds, against spam (the hub keeps the same rule)
	auto sendMessage = [&](const std::string& text) {
		std::string clean;
		for (char c : text) if (c != '\n' && c != '\r') clean += c;
		if (clean.empty()) return;
		clean = clean.substr(0, 120);
		bool sticker = false;
		for (const char* e : kEmojis) sticker |= clean.find(std::string(":") + e + ":") != std::string::npos;
		if (sticker) {
			Uint64 now = SDL_GetTicksNS();
			if (stickerAt && now - stickerAt < 10000000000ULL) {
				note = "Un sticker cada 10 segundos";
				noteAt = now;
				return;
			}
			stickerAt = now;
		}
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
			if (g_ctl.Event(ev)) continue;
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
				case SDL_SCANCODE_F1: g_ctl.Open(); break;
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
		if (!g_ctl.open) ReadPlayers(in, !panel.open);
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
				{ std::lock_guard<std::mutex> lock(g_namesLock); g_names[0] = "yoy"; g_ranks[0] = "Naboria"; g_names[1] = "ELGINGIRIANO"; g_ranks[1] = "";
				  g_photos[0] = std::string(SDL_GetBasePath()) + "emojis/ggs.png"; }   // a picture as yoy's photo
				g_score[0] = 2; g_score[1] = 1;
				g_watchers = 2;
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
		streamFrames(false);
		const NetStats& stats = net.Stats();
		// with voice chat in the set, the game's sound goes to half, so the players hear each other
		static float gain = 1.0f;
		float wantGain = g_voiceRival >= 0 ? 0.5f : 1.0f;
		if (g_aud && wantGain != gain) { SDL_SetAudioStreamGain(g_aud, wantGain); gain = wantGain; }
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
		if (st == NetMatch::Waiting) banner = "CONECTANDO\n" + net.RouteText() + "   (Esc = salir)";
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
			if (st == NetMatch::Playing || st == NetMatch::Ended) { DrawNames(r); DrawWatchers(r); }
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
	streamFrames(true);
	const NetStats& s = net.Stats();
	if (!recordTo.empty() && s.started) {   // the set's replay (the app sends it to the hub)
		Replay rep = net.Recording();
		printf("repeticion %s %zu\n", SaveReplay(recordTo, rep) ? "guardada" : "no-guardada", rep.frames.size());
	}
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
static int PercentOfLife(int points) { return (points * 100 + kFullLife / 2) / kFullLife; }   // of a full bar, rounded

// The combo counter, under the player's life bar past the button history:
// "3 GOLPES" big and the damage (and how much of a full life bar it is).
// A finished combo stays a while, dimmer.
static void DrawCombo(const SDL_FRect& r, const ComboCounter& c)
{
	if (c.hits < 1 || (!c.live && c.shownFor <= 0)) return;
	float s = r.h / 450 > 1 ? r.h / 450 : 1, big = s * 1.75f;
	char hits[32], dmg[48];
	snprintf(hits, sizeof(hits), "%d %s", c.hits, c.hits == 1 ? "GOLPE" : "GOLPES");
	snprintf(dmg, sizeof(dmg), "DANO %d  (%d%%)", c.damage, PercentOfLife(c.damage));
	float w = SDL_max(strlen(hits) * 8 * big, strlen(dmg) * 8 * s) + 16 * s, h = 36 * s;
	float x = r.x + 118 * s, y = r.y + 68 * s;   // clear of the clock in the middle
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, c.live ? 150 : 100);
	SDL_FRect b{ x, y, w, h };
	SDL_RenderFillRect(g_ren, &b);
	Uint8 k = c.live ? 255 : 170;
	Text(x + 8 * s, y + 6 * s, big, hits, k, c.live ? 220 : 150, c.live ? 80 : 60);
	Text(x + 8 * s, y + 26 * s, s, dmg, k, k, k);
}

// Under the rival's life bar, always there: its life in points (the bar has no
// numbers) and the best combo the player did on it in this fight, which stays.
static void DrawRivalBox(const SDL_FRect& r, const ComboCounter& c)
{
	float s = r.h / 450 > 1 ? r.h / 450 : 1, ts = s * 0.9f;
	int life = SDL_max((int)(int16_t)ReadWord(kP2 + kLife), 0);
	char l1[40], l2[40];
	snprintf(l1, sizeof(l1), "VIDA %d / %d", life, kFullLife);
	if (c.bestHits) snprintf(l2, sizeof(l2), "%d %s  %d (%d%%)", c.bestHits, c.bestHits == 1 ? "GOLPE" : "GOLPES", c.bestDamage, PercentOfLife(c.bestDamage));
	else snprintf(l2, sizeof(l2), "todavia ninguno");
	float w = SDL_max(SDL_max(strlen(l1) * 8 * s * 1.25f, strlen(l2) * 8 * ts), 11 * 8 * ts) + 16 * s, h = 44 * s;
	float x = r.x + r.w - 106 * s - w, y = r.y + 68 * s;   // between the clock and the rival's portrait
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 140);
	SDL_FRect b{ x, y, w, h };
	SDL_RenderFillRect(g_ren, &b);
	Text(x + 8 * s, y + 5 * s, s * 1.25f, l1, 255, 255, 255);
	Text(x + 8 * s, y + 20 * s, ts, "MEJOR COMBO", 150, 200, 255);
	Text(x + 8 * s, y + 31 * s, ts, l2, c.bestHits ? 255 : 160, c.bestHits ? 220 : 160, c.bestHits ? 80 : 160);
}

static void DrawHistory(const SDL_FRect& r, const std::deque<HistoryEntry>& h, bool right, const char* title)
{
	// narrow (as wide as its widest row) and see-through, so it hides little
	float s = r.h / 450 > 1 ? r.h / 450 : 1, ts = s * 0.85f, row = 13 * s;
	int most = 0;
	for (auto& e : h) {
		int n = 0;
		for (int b = 0; b < 6; b++) n += (e.in & (GUASA_LP << b)) != 0;
		most = SDL_max(most, n);
	}
	float buttonsAt = 3 * 8 * ts + 20 * s, w = SDL_max(buttonsAt + most * 17 * ts + 4 * s, 40 * s);
	float x = right ? r.x + r.w - w - 6 * s : r.x + 6 * s, y = r.y + 64 * s;
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 112);
	SDL_FRect box{ x, y - 13 * s, w, row * h.size() + 16 * s };
	SDL_RenderFillRect(g_ren, &box);
	Text(x + 3 * s, y - 10 * s, ts, title, 255, 220, 120);
	static const char* names[6] = { "LP", "MP", "HP", "LK", "MK", "HK" };
	for (size_t i = 0; i < h.size(); i++) {
		float ry = y + i * row;
		char n[8];
		snprintf(n, sizeof(n), "%3d", h[i].frames);
		Text(x + 3 * s, ry + 3 * s, ts, n, 170, 170, 170);
		Arrow(x + 3 * 8 * ts + 11 * s, ry + 6 * s, 4 * s, h[i].in, 255, 255, 255);
		float bx = x + buttonsAt;
		for (int b = 0; b < 6; b++)
			if (h[i].in & (GUASA_LP << b)) {   // only the pressed ones, one after the other
				Text(bx, ry + 3 * s, ts, names[b], b < 3 ? 120 : 255, b < 3 ? 200 : 140, b < 3 ? 255 : 120);
				bx += 17 * ts;
			}
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

// Practice's own stage: the game draws only its sprites (fighters, fireballs and
// the screen's bars) on black; behind them, a light room with a floor and lines
// that move with the camera like a stage would: near ones every 32 pixels of the
// stage (stronger every 128), far faint ones at half speed (depth), and lines
// every 32 pixels up from the floor to measure jumps.
static void DrawOwnStage(const SDL_FRect& r)
{
	auto s16 = [](uint32_t a) { return (int)(int16_t)(guasa_read_ram(a) << 8 | guasa_read_ram(a + 1)); };
	int left = s16(0xFF8ED4), top = s16(0xFF8ED8);
	// the floor: where a fighter on the ground stands (the last one seen on it)
	static int groundY = 40;
	for (uint32_t p : { kP1, kP2 }) if (guasa_read_ram(p + 3) != 0x04) groundY = s16(p + 0x0A);
	float kx = r.w / 384, ky = r.h / 224;
	float floorY = r.y + (224 - (groundY - 15) + top) * ky;
	auto line = [&](float x0, float y0, float x1, float y1, Uint8 c, Uint8 a) {
		SDL_SetRenderDrawColor(g_ren, c, c, (Uint8)SDL_min(c + 10, 255), a);
		SDL_RenderLine(g_ren, x0, y0, x1, y1);
	};
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 236, 238, 243, 255);   // the room
	SDL_RenderFillRect(g_ren, &r);
	for (int k = -1; k < 384 / 64 + 2; k++) {   // far: half the camera's speed
		float x = r.x + (k * 64 - (left / 2) % 64) * kx;
		line(x, r.y, x, floorY, 222, 255);
	}
	SDL_SetRenderDrawColor(g_ren, 222, 225, 232, 255);   // the floor
	SDL_FRect floor{ r.x, floorY, r.w, r.y + r.h - floorY };
	SDL_RenderFillRect(g_ren, &floor);
	for (int k = -1; k < 384 / 32 + 2; k++) {   // near: with the stage, every 32 pixels
		int wx = (left / 32 + k) * 32;
		float x = r.x + (wx - left) * kx;
		bool strong = wx % 128 == 0;
		line(x, floorY, x, r.y + r.h, strong ? 150 : 190, 255);
		line(x, r.y + 24 * ky, x, floorY, strong ? 200 : 214, 255);
	}
	for (int h = 32; h < 200; h += 32) {   // heights above the floor
		float y = floorY - h * ky;
		if (y > r.y + 20 * ky) line(r.x, y, r.x + r.w, y, 214, 255);
	}
	SDL_SetRenderDrawColor(g_ren, 120, 128, 146, 255);   // the floor's edge, two pixels
	SDL_RenderLine(g_ren, r.x, floorY, r.x + r.w, floorY);
	SDL_RenderLine(g_ren, r.x, floorY + 1, r.x + r.w, floorY + 1);
	// the game's picture on top, its black see-through
	static std::vector<uint32_t> px;
	int w, h, pitch;
	const uint16_t* fb = guasa_framebuffer(&w, &h, &pitch);
	px.resize((size_t)w * h);
	for (int y = 0; y < h; y++) {
		const uint16_t* row = (const uint16_t*)((const uint8_t*)fb + y * pitch);
		for (int x = 0; x < w; x++) {
			uint16_t c = row[x];
			px[(size_t)y * w + x] = c == 0 ? 0 : 0xFF000000u | (uint32_t)((c >> 11) << 3 | (c >> 13)) << 16 |
				(uint32_t)(((c >> 5) & 63) << 2 | ((c >> 9) & 3)) << 8 | (uint32_t)((c & 31) << 3 | ((c >> 2) & 7));
		}
	}
	if (!g_texSprites) {
		g_texSprites = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
		SDL_SetTextureScaleMode(g_texSprites, SDL_SCALEMODE_NEAREST);
		SDL_SetTextureBlendMode(g_texSprites, SDL_BLENDMODE_BLEND);
	}
	SDL_UpdateTexture(g_texSprites, NULL, px.data(), w * 4);
	SDL_RenderTexture(g_ren, g_texSprites, NULL, &r);
}

static void DrawPractice(const Practice& P, const std::vector<MenuItem>* menu, int sel, const std::string& note)
{
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
	SDL_RenderClear(g_ren);
	SDL_FRect r = GameRect();
	bool own = P.stage == kOwnStage && P.phase == Practice::Fight;
	guasa_set_layers(own ? 0x01 : 0xFF);   // its own stage: only the game's sprites (from the next frame)
	if (own) DrawOwnStage(r);
	else {
		SDL_UpdateTexture(g_tex, NULL, guasa_framebuffer(NULL, NULL, NULL), g_w * 2);
		SDL_RenderTexture(g_ren, g_tex, NULL, &r);
	}
	float s = r.h / 450 > 1 ? r.h / 450 : 1;
	SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 160);
	SDL_FRect top{ r.x, r.y, r.w, 14 * s }, bottom{ r.x, r.y + r.h - 14 * s, r.w, 14 * s };
	SDL_RenderFillRect(g_ren, &top);
	SDL_RenderFillRect(g_ren, &bottom);
	Text(r.x + 6 * s, r.y + 3 * s, s, P.Status().c_str(), 255, 220, 120);
	Text(r.x + 6 * s, r.y + r.h - 11 * s, s, "MONEDA, Tab o clic derecho: menu    START: volver a la posicion    Esc: salir", 200, 200, 200);
	if ((P.showBoxes || P.showPush) && P.phase == Practice::Fight) {   // hurt blue, attack red, push green
		float kx = r.w / 384, ky = r.h / 224;
		SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
		for (const Box& b : P.ShownBoxes()) {
			if (b.kind == kBoxPush ? !P.showPush : !P.showBoxes) continue;
			static const Uint8 c[3][3] = { { 60, 130, 255 }, { 255, 40, 40 }, { 60, 220, 90 } };
			const Uint8* k = c[b.kind];
			SDL_FRect f{ r.x + b.l * kx, r.y + b.t * ky, (b.r - b.l) * kx, (b.b - b.t) * ky };
			SDL_SetRenderDrawColor(g_ren, k[0], k[1], k[2], 45);
			SDL_RenderFillRect(g_ren, &f);
			SDL_SetRenderDrawColor(g_ren, k[0], k[1], k[2], 255);
			SDL_RenderRect(g_ren, &f);
			SDL_FRect in{ f.x + 1, f.y + 1, f.w - 2, f.h - 2 };   // two pixels thick
			SDL_RenderRect(g_ren, &in);
		}
	}
	if (P.showFrames && P.phase == Practice::Fight) DrawFrameMeter(r, P.meter);
	if (P.showCombo && P.phase == Practice::Fight) { DrawCombo(r, P.combo); DrawRivalBox(r, P.combo); }
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
	g_ctl.Draw();
	SDL_RenderPresent(g_ren);
}

// The practice options are kept for next time (practica.txt), one "name value" a line.
static void PracticeOptions(Practice& P, bool save)
{
	struct { const char* name; int* i; bool* b; } opts[] = {
		{ "rival", &P.stance, NULL }, { "delay", &P.delay, NULL }, { "vida", NULL, &P.refillLife }, { "reloj", NULL, &P.freezeClock },
		{ "historial", NULL, &P.showHistory }, { "medidor", NULL, &P.showFrames },
		{ "cajas", NULL, &P.showBoxes }, { "empuje", NULL, &P.showPush }, { "combos", NULL, &P.showCombo },
		{ "levantarse", &P.onWake, NULL }, { "bloquear", &P.onBlock, NULL },
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
		if (P.onWake < 0 || P.onWake >= kReactions) P.onWake = kReactNothing;
		if (P.onBlock < 0 || P.onBlock >= kReactions) P.onBlock = kReactNothing;
	}
	fclose(f);
}

// For checking the screen without hands: "captura=<folder>" plays a fixed script
// (pick, walk up, a few hits, a combo) and saves practica-combo.bmp, practica-pelea.bmp
// and practica-menu.bmp.
static void SaveWindow(const std::string& path)
{
	SDL_Surface* s = SDL_RenderReadPixels(g_ren, NULL);
	if (s) { SDL_SaveBMP(s, path.c_str()); SDL_DestroySurface(s); }
}

// stage: chosen in the app before opening (the game loads it with the fight, so
// it isn't changed from inside): 0x0C Cammy's, kOwnStage practice's own, -1 the game's.
static int PlayPractice(const char* roms, const std::string& shots, int stage)
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
	P.stage = stage;
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
			item("Al levantarse", ReactionName(P.onWake), "", [&](int d) { P.onWake = (P.onWake + d + kReactions) % kReactions; }),
			item("Despues de bloquear", ReactionName(P.onBlock), "", [&](int d) { P.onBlock = (P.onBlock + d + kReactions) % kReactions; }),
			item(P.rec == Practice::Recording ? "Parar la grabacion" : "Grabarlo (tu lo mueves)", "", "F5", [&](int) { P.ToggleRecord(); menuOpen = false; }),
			item("Repetir la grabacion", P.tape.empty() ? "no hay" : yesNo(P.replay), "F6", [&](int) { if (!P.tape.empty()) P.replay = !P.replay; }),
			item("Moverlo tu mismo", yesNo(P.controlRival), "F3", [&](int) { if (P.rec == Practice::NoRec) P.controlRival = !P.controlRival; }),
			item("Con otro control", g_devs.size() < 2 ? "no hay" : yesNo(secondControl), "", [&](int) { secondControl = !secondControl && g_devs.size() >= 2; }),
			Header("AYUDAS"),
			item("Vida que se recupera", yesNo(P.refillLife), "", [&](int) { P.refillLife = !P.refillLife; }),
			item("Tiempo infinito", yesNo(P.freezeClock), "", [&](int) { P.freezeClock = !P.freezeClock; }),
			item("Delay como en linea", std::to_string(P.delay), "F9", [&](int d) { P.delay = (P.delay + d + 6) % 6; }),
			Header("PANTALLA"),
			item("Historial de botones", yesNo(P.showHistory), "F2", [&](int) { P.showHistory = !P.showHistory; }),
			item("Cajas de golpe", yesNo(P.showBoxes), "", [&](int) { P.showBoxes = !P.showBoxes; }),
			item("Cajas de empuje", yesNo(P.showPush), "", [&](int) { P.showPush = !P.showPush; }),
			item("Medidor de frames", yesNo(P.showFrames), "F12", [&](int) { P.showFrames = !P.showFrames; }),
			item("Contador de combos", yesNo(P.showCombo), "", [&](int) { P.showCombo = !P.showCombo; }),
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
			if (g_ctl.Event(ev)) continue;
			switch (ev.type) {
			case SDL_EVENT_QUIT: quit = true; break;
			case SDL_EVENT_JOYSTICK_ADDED: AddDevice(ev.jdevice.which); break;
			case SDL_EVENT_JOYSTICK_REMOVED: RemoveDevice(ev.jdevice.which); break;
			case SDL_EVENT_KEY_DOWN:
				if (ev.key.repeat) break;
				switch (ev.key.scancode) {
				case SDL_SCANCODE_ESCAPE: if (menuOpen) menuOpen = false; else quit = true; break;
				case SDL_SCANCODE_TAB: openMenu(!menuOpen); break;
				case SDL_SCANCODE_F1: g_ctl.Open(); break;
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
		if (!g_ctl.open) ReadPlayers(in);
		if (!shots.empty()) {   // the script: pick both, walk up, hit, then the menu
			static int t = 0, fightAt = -1;
			t++;
			in[0] = in[1] = 0;
			if ((t > 30 && t < 34) || (t > 64 && t < 68)) in[0] = GUASA_LP;
			if (P.phase == Practice::Fight && fightAt < 0) fightAt = t;
			int k = fightAt < 0 ? -1 : t - fightAt;
			if (k >= 0 && k < 80) in[0] = GUASA_RIGHT;
			if (k >= 80 && k < 140 && k % 24 < 3) in[0] = k % 48 < 24 ? GUASA_DOWN | GUASA_MK : GUASA_HP;
			if (k >= 140 && k < 165) in[0] = GUASA_RIGHT;   // close again
			if (k >= 165 && k < 174) in[0] = k < 168 ? GUASA_DOWN | GUASA_MK : k < 171 ? GUASA_DOWN | GUASA_RIGHT : GUASA_RIGHT | GUASA_HP;   // a combo
			if (k == 222) { DrawPractice(P, NULL, 0, ""); SaveWindow(shots + "/practica-combo.bmp"); }   // once the life bar finished going down
			if (k >= 200 && k < 215) in[0] = GUASA_DOWN | GUASA_RIGHT;
			for (int g : { 108, 110, 112 }) if (k == g) { DrawPractice(P, NULL, 0, ""); SaveWindow(shots + "/practica-golpe-" + std::to_string(g) + ".bmp"); }
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
			std::string banner = "";
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





// sonido-reto <roms> <out.wav>: the arcade's "Here comes a new challenger!" sound,
// from the player's own ROM and on their PC (nothing of the game comes with the
// app): from power-on, a coin and start for player 1, a fight against the CPU,
// and player 2 joins. The game cuts the music and plays it (found comparing the
// sound with and without player 2 joining): its 2.3 seconds go to a WAV, which
// the app plays when a challenge arrives. No window; about a second.
static int ChallengerSound(const char* roms, const char* out)
{
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) { printf("error: %s\n", msg); return 1; }
	const int join = 3200, from = join + 2, len = 138;   // player 2's start, and the frames of the sound
	std::vector<int16_t> pcm;
	for (int f = 0; f < from + len; f++) {
		uint16_t in[2] = { 0, 0 };
		if (f >= 1500 && f < 1506) in[0] = GUASA_COIN;
		if (f >= 1600 && f < 1606) in[0] = GUASA_START;
		if (f >= 2000 && f < 2006) in[0] = GUASA_LP;   // Ryu, and the fight starts
		if (f >= join - 80 && f < join - 74) in[1] = GUASA_COIN;
		if (f >= join && f < join + 6) in[1] = GUASA_START;
		guasa_run_frame(in, 0);
		if (f >= from) { int n = 0; const int16_t* a = guasa_audio(&n); pcm.insert(pcm.end(), a, a + n * 2); }
	}
	guasa_exit();
	// a short fade at the end, so it doesn't click
	size_t fade = SDL_min(pcm.size() / 2, (size_t)GUASA_SOUND_RATE / 10);
	for (size_t k = 0; k < fade; k++)
		for (int c = 0; c < 2; c++) {
			int16_t& v = pcm[pcm.size() - 2 * (fade - k) + c];
			v = (int16_t)(v * (double)(fade - k) / fade);
		}
	double sum = 0;
	for (int16_t v : pcm) sum += (double)v * v;
	if (pcm.empty() || SDL_sqrt(sum / pcm.size()) < 200) { printf("error: no se oyo nada\n"); return 1; }   // silence: something went wrong
	FILE* f = fopen(out, "wb");
	if (!f) { printf("error: no pude escribir %s\n", out); return 1; }
	uint32_t data = (uint32_t)pcm.size() * 2, rate = GUASA_SOUND_RATE;
	auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); u32(36 + data); fwrite("WAVEfmt ", 1, 8, f);
	u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
	fwrite("data", 1, 4, f); u32(data);
	fwrite(pcm.data(), 2, pcm.size(), f);
	fclose(f);
	printf("listo\n");
	return 0;
}



static int ControlsScreen(const std::string& shots)
{
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
	g_win = SDL_CreateWindow("La Guasabara - Controles", (int)ControlsPanel::W, (int)ControlsPanel::H, SDL_WINDOW_RESIZABLE);
	g_ren = g_win ? SDL_CreateRenderer(g_win, NULL) : NULL;
	if (!g_ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
	SDL_SetRenderVSync(g_ren, 1);
	LoadMaps();
	printf("controles\n");
	fflush(stdout);
	g_ctl.Open();
	int frame = 0;
	while (g_ctl.open) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) g_ctl.open = false;
			if (ev.type == SDL_EVENT_JOYSTICK_ADDED) AddDevice(ev.jdevice.which);
			if (ev.type == SDL_EVENT_JOYSTICK_REMOVED) RemoveDevice(ev.jdevice.which);
			g_ctl.Event(ev);
		}
		SDL_SetRenderDrawColor(g_ren, 13, 14, 19, 255);
		SDL_RenderClear(g_ren);
		g_ctl.Draw();
		if (!shots.empty()) {   // checking without hands: the keyboard's heavy punch, then Q pressed for it
			if (frame == 2) g_ctl.pick("teclado");
			if (frame == 20) SaveWindow(shots + "/controles.bmp");
			if (frame == 25) { g_ctl.listen = g_ctl.focus = 6; }
			if (frame == 31) SaveWindow(shots + "/controles-esperando.bmp");
			if (frame == 33) { SDL_Event e{}; e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_Q; SDL_PushEvent(&e); }
			if (frame == 40) { printf("puno fuerte: %s\n", BindText(g_ctl.cur.b[6]).c_str()); fflush(stdout); SaveWindow(shots + "/controles-asignado.bmp"); g_ctl.open = false; }
		}
		frame++;
		SDL_RenderPresent(g_ren);
	}
	printf("controles listos\n");
	fflush(stdout);
	for (auto& dv : g_devs) { if (dv.pad) SDL_CloseGamepad(dv.pad); SDL_CloseJoystick(dv.joy); }
	g_devs.clear();
	SDL_Quit();
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

	// nobody picks: the select screen's timer does, and practice starts all the same
	{
		int f = 0;
		while (P.phase != Practice::Fight && f++ < 6000) { uint16_t out[2]; P.Inputs(0, -1, out); guasa_run_frame(out, 0); P.AfterFrame(out); }
		check(P.phase == Practice::Fight && P.HasPosition(), "si el reloj de la seleccion elige solo, la practica empieza igual", std::to_string(f) + " cuadros");
		P.ChangeCharacters();
	}
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

	run(30, 0); run(4, GUASA_LP); run(10, 0);
	{
		// the old version's code: right after picking, the directions still go to the player
		uint16_t got = frame(GUASA_LEFT);
		check(P.phase == Practice::PickMine && (got & GUASA_LEFT), "despues de elegir, las direcciones del codigo old siguen siendo tuyas");
	}
	run(110, 0);
	check(P.phase == Practice::PickRival, "con el primer golpe se elige tu personaje y sigue el del rival");
	int f = pickRival(GUASA_RIGHT), rivalRight = char2();
	check(P.phase == Practice::Fight && P.HasPosition(), "empieza la pelea y queda guardada la posicion", std::to_string(f) + " cuadros");
	check(ReadWord(kStage) == 0x0C, "la pelea es en el escenario de Cammy", std::to_string(ReadWord(kStage)));

	// an attack against a rival that blocks, and against one that stands
	auto attack = [&](int stance) {
		P.LoadPosition();
		P.stance = stance;
		int least = life2();
		run(100, GUASA_RIGHT);   // up against the rival
		for (int k = 0; k < 12; k++) { run(3, GUASA_HP); run(20, 0); least = std::min(least, life2()); }
		return least;
	};
	// blocking without being attacked: it stays where it is (holding back all the
	// time would walk it off to the corner)
	P.LoadPosition();
	P.stance = kBlock;
	int before = x2();
	run(120, 0);
	check(x2() == before, "el rival que bloquea no se va caminando si no lo atacan", std::to_string(before) + " -> " + std::to_string(x2()));
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

	// the boxes: each player has body boxes, player 1's left of player 2's, and a
	// heavy punch up close has an attack box on the rival
	{
		P.LoadPosition();
		std::vector<Box> b = ComputeBoxes();
		int hurt[2] = { 0, 0 }, mid[2] = { 0, 0 };
		for (const Box& x : b) if (x.kind == kBoxHurt) { int p = (x.l + x.r) / 2 < 192 ? 0 : 1; hurt[p]++; mid[p] += (x.l + x.r) / 2; }
		check(hurt[0] >= 3 && hurt[1] >= 3, "cajas: cada jugador tiene sus cajas de cuerpo (cabeza, cuerpo, piernas)",
		      std::to_string(hurt[0]) + " y " + std::to_string(hurt[1]));
		run(100, GUASA_RIGHT);
		run(20, 0);
		bool attack = false, onRival = false;
		for (int k = 0; k < 20 && !onRival; k++) {
			frame(k < 3 ? GUASA_HP : 0);
			std::vector<Box> now = ComputeBoxes();
			for (const Box& a : now) if (a.kind == kBoxAttack) {
				attack = true;
				for (const Box& h : now)   // overlapping one of the rival's (the right one's) body boxes
					if (h.kind == kBoxHurt && (h.l + h.r) / 2 > (a.l + a.r) / 2 && a.r >= h.l && a.l <= h.r && a.b >= h.t && a.t <= h.b) onRival = true;
			}
		}
		check(attack && onRival, "cajas: el golpe fuerte de cerca tiene caja roja sobre el rival");
	}

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

	// the combo counter: what it says after each of these (the player is Ryu)
	auto combo = [&](int stance, const std::vector<std::pair<int, uint16_t>>& steps, bool close = true) {
		P.LoadPosition();
		P.stance = stance;
		if (close) { run(100, GUASA_RIGHT); run(20, 0); }
		P.combo.Reset();
		P.combo.hits = P.combo.damage = 0;
		for (auto [n, b] : steps) run(n, b);
		run(150, 0);
		const ComboCounter& c = P.combo;
		return std::make_pair(c.hits, std::to_string(c.hits) + " golpes, " + std::to_string(c.damage) + " de dano" + (c.live ? ", sigue" : ""));
	};
	auto [h1, d1] = combo(kStand, { { 3, GUASA_HP } });
	check(h1 == 1 && !P.combo.live, "combos: un golpe fuerte es 1 golpe, y termina", d1);
	auto [h2, d2] = combo(kStand, { { 6, GUASA_DOWN }, { 3, GUASA_DOWN | GUASA_MK }, { 3, GUASA_DOWN | GUASA_RIGHT }, { 3, GUASA_RIGHT | GUASA_HP } });
	check(h2 == 2 && P.combo.damage > 24, "combos: barrida media cancelada en hadoken, 2 golpes", d2);
	int bestDamage = P.combo.damage;
	auto [h3, d3] = combo(kStand, { { 4, GUASA_RIGHT | GUASA_HP } });
	check(h3 == 1 && P.combo.damage > 0, "combos: el agarre cuenta como golpe", d3);
	auto [h4, d4] = combo(kStand, { { 3, GUASA_HP }, { 90, 0 }, { 3, GUASA_HP } });
	check(h4 == 1, "combos: dos golpes con tiempo entre medio no son combo", d4);
	P.refillLife = false;
	auto [h5, d5] = combo(kBlock, { { 3, GUASA_DOWN }, { 3, GUASA_DOWN | GUASA_RIGHT }, { 3, GUASA_RIGHT | GUASA_LP } }, false);
	int chip = kFullLife - life2();
	P.refillLife = true;
	check(h5 == 0, "combos: un hadoken bloqueado no es golpe (aunque raspe vida)", d5 + ", raspo " + std::to_string(chip));
	check(P.combo.bestHits == 2 && P.combo.bestDamage == bestDamage, "combos: el mejor combo se queda (el de mas dano)",
	      std::to_string(P.combo.bestHits) + " golpes, " + std::to_string(P.combo.bestDamage) + " de dano");
	// the rival hitting the player (moved by the player here) doesn't count
	{
		P.LoadPosition();
		P.controlRival = true;
		run(100, GUASA_LEFT);   // the rival walks up to the player
		run(20, 0);
		P.combo.Reset();
		P.combo.hits = 0;
		int least = kFullLife;
		for (int k = 0; k < 3; k++) { run(3, GUASA_HP); for (int i = 0; i < 40; i++) { frame(0); least = SDL_min(least, (int)(int16_t)ReadWord(kP1 + kLife)); } }
		P.controlRival = false;
		check(least < kFullLife && P.combo.hits == 0, "combos: los golpes del rival al jugador no cuentan",
		      "vida del jugador " + std::to_string(least) + ", golpes contados " + std::to_string(P.combo.hits));
	}

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
	run(30, 0); run(4, GUASA_LP); run(110, 0);
	pickRival(GUASA_LEFT);
	int rivalLeft = char2();
	check(P.phase == Practice::Fight && rivalLeft != rivalRight, "el rival es el que elige el jugador, no el del cursor",
	      "derecha " + std::to_string(rivalRight) + ", izquierda " + std::to_string(rivalLeft));

	// reversals, Ryu against Ken: after the rival's last stunned frame (knocked
	// down, or blocking), its move must start on the very next one
	P.ChangeCharacters();
	run(30, 0); run(4, GUASA_LP); run(110, 0);
	pickRival(0);
	check(char2() == 4, "reversal: el rival es Ken", std::to_string(char2()));
	auto watch = [&](const std::vector<std::pair<int, uint16_t>>& attack, int stance) {
		P.LoadPosition();
		P.stance = stance;
		run(100, GUASA_RIGHT); run(20, 0);
		for (auto [n, b] : attack) run(n, b);
		int last = -1, move = -1, what = 0;
		for (int i = 0; i < 200 && move < 0; i++) {
			frame(0);
			uint8_t st = guasa_read_ram(kP2 + 3);
			if (st == 0x0E || st == 0x14) last = i;
			else if (last >= 0 && (st == 0x0A || st == 0x0C || st == 0x04)) { move = i; what = st; }
		}
		return std::make_pair(move < 0 ? -1 : move - last, what);
	};
	auto said = [](std::pair<int, int> r) {
		return r.first < 0 ? std::string("no hizo nada") : "a los " + std::to_string(r.first) + " cuadros, estado " + std::to_string(r.second);
	};
	const std::vector<std::pair<int, uint16_t>> sweep = { { 4, GUASA_DOWN | GUASA_HK } }, heavy = { { 3, GUASA_HP } };
	P.onWake = kReactHeavy;
	auto r1 = watch(sweep, kStand);
	// a normal: on the first free frame or the next, as the game lets it (specials and jumps, always on the first)
	check(r1.first >= 1 && r1.first <= 2 && r1.second == 0x0A, "reversal: al levantarse, golpe fuerte lo antes que deja el juego", said(r1));
	P.onWake = kReactJumpBack;
	auto r2 = watch(sweep, kStand);
	check(r2.first == 1 && r2.second == 0x04, "reversal: al levantarse, salta atras en el primer cuadro libre", said(r2));
	// a shoryuken recorded by the player (Ken on the right: forward is left)
	P.LoadPosition();
	P.ToggleRecord();
	run(10, 0); run(1, GUASA_LEFT); run(1, GUASA_DOWN); run(2, GUASA_DOWN | GUASA_LEFT | GUASA_HP); run(40, 0);
	P.ToggleRecord();
	P.replay = false;
	P.onWake = kReactTape;
	auto r3 = watch(sweep, kStand);
	check(r3.first == 1 && r3.second == 0x0C, "reversal: al levantarse, el shoryuken grabado sale en el primer cuadro libre", said(r3));
	P.onWake = kReactNothing;
	P.onBlock = kReactTape;
	auto r4 = watch(heavy, kBlock);
	check(r4.first == 1 && r4.second == 0x0C, "reversal: despues de bloquear, el shoryuken grabado sale en el primer cuadro libre", said(r4));
	P.onBlock = kReactNothing;
	auto r5 = watch(sweep, kStand);
	check(r5.first < 0, "reversal: en «nada», el rival no hace nada", said(r5));

	// the super meter is always full: Ryu's super comes out (its fireball hits several
	// times; a hadoken, once), and again right after
	check(guasa_read_ram(kP1 + kSuper) == kFullSuper && guasa_read_ram(kP2 + kSuper) == kFullSuper, "la super esta llena");
	auto superHits = [&] {
		P.LoadPosition();
		P.stance = kStand;
		P.combo.Reset();
		P.combo.hits = 0;
		const uint16_t motion[] = { GUASA_DOWN, GUASA_DOWN | GUASA_RIGHT, GUASA_RIGHT, GUASA_DOWN, GUASA_DOWN | GUASA_RIGHT, GUASA_RIGHT | GUASA_LP };
		for (uint16_t x : motion) run(2, x);
		run(150, 0);
		return P.combo.hits;
	};
	int s1 = superHits(), s2 = superHits();
	check(s1 >= 3 && s2 >= 3, "la super sale siempre (su bola de fuego pega varias veces)", std::to_string(s1) + " y " + std::to_string(s2) + " golpes");
	guasa_exit();
	return fails ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Replays: the buttons of every frame, played again from the starting state.

// repeticion-prueba: the whole replay without a window; its final state, to
// compare with the game that recorded it.
static int ReplayCheck(const char* roms, const char* file)
{
	Replay rep;
	std::string err;
	if (!LoadReplay(file, rep, &err)) { printf("error: %s\n", err.c_str()); return 1; }
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) { printf("error: %s\n", msg); return 1; }
	guasa_boot_to_select();
	PauseState ps;
	for (size_t i = 0; i < rep.frames.size(); i++) ReplayFrame(rep, i, ps, false);
	printf("repeticion: %zu cuadros, estado final %016llx\n", rep.frames.size(), (unsigned long long)guasa_state_hash());
	guasa_exit();
	return 0;
}

static std::string Clock(size_t frames)
{
	int secs = (int)(frames / guasa_fps());
	char b[16];
	snprintf(b, sizeof(b), "%d:%02d", secs / 60, secs % 60);
	return b;
}

// repeticion: watching one. Space (or START) pauses, F changes the speed (x1, x2,
// x4), left / right go 10 seconds back / forward, F2 shows both players' buttons,
// Esc leaves. Going back loads the nearest picture of the game kept every 10 s.
// shots: "captura=<folder>" (checking the screen without hands): a picture while
// playing and one paused with the buttons shown, then it closes.
static int PlayReplay(const char* roms, const char* file, const std::string& shots = "")
{
	Replay rep;
	std::string err;
	if (!LoadReplay(file, rep, &err)) { printf("error: %s\n", err.c_str()); return 1; }
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) {
		char report[4000] = "";
		guasa_check_rom(roms, report, sizeof(report));
		printf("error: %s\n%s", msg, report);
		return 1;
	}
	guasa_boot_to_select();
	if (!OpenWindow("La Guasabara - Repeticion")) return 1;
	LoadMaps();
	printf("repeticion\n");
	fflush(stdout);

	const size_t kEvery = (size_t)(10 * guasa_fps() + 0.5);
	struct Mark { std::vector<uint8_t> state; PauseState pause; };
	std::vector<Mark> marks;   // marks[k]: the game just before frame k * kEvery
	size_t i = 0;
	PauseState ps;
	auto mark = [&]() {
		if (i % kEvery || i / kEvery < marks.size()) return;
		Mark m;
		m.state.resize(guasa_state_size());
		guasa_save(m.state.data(), m.state.size());
		m.pause = ps;
		marks.push_back(std::move(m));
	};
	// to frame `to`: from the nearest mark before it, without drawing (fast)
	auto seek = [&](size_t to) {
		to = SDL_min(to, rep.frames.size());
		if (to < i) {
			size_t k = SDL_min(to / kEvery, marks.size() - 1);
			guasa_load(marks[k].state.data(), marks[k].state.size());
			ps = marks[k].pause;
			i = k * kEvery;
		}
		while (i < to) { mark(); ReplayFrame(rep, i, ps, i + 1 == to); i++; }
	};

	bool quit = false, paused = false, showInputs = false;
	int speed = 1;
	uint16_t lastPad = 0;
	const Uint64 frameNs = (Uint64)(1e9 / guasa_fps());
	Uint64 next = SDL_GetTicksNS();
	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (g_ctl.Event(ev)) continue;
			switch (ev.type) {
			case SDL_EVENT_QUIT: quit = true; break;
			case SDL_EVENT_JOYSTICK_ADDED: AddDevice(ev.jdevice.which); break;
			case SDL_EVENT_JOYSTICK_REMOVED: RemoveDevice(ev.jdevice.which); break;
			case SDL_EVENT_KEY_DOWN:
				switch (ev.key.scancode) {
				case SDL_SCANCODE_ESCAPE: quit = true; break;
				case SDL_SCANCODE_SPACE: if (!ev.key.repeat) paused = !paused; break;
				case SDL_SCANCODE_F: if (!ev.key.repeat) speed = speed == 4 ? 1 : speed * 2; break;
				case SDL_SCANCODE_LEFT: seek(i > kEvery ? i - kEvery : 0); break;
				case SDL_SCANCODE_RIGHT: seek(i + kEvery); break;
				case SDL_SCANCODE_F2: showInputs = !showInputs; break;
				default: break;
				}
				if (ev.key.scancode == SDL_SCANCODE_F11 || (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)))
					SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
				break;
			}
		}
		// with a control: START pauses, left / right jump, a punch changes the speed
		uint16_t in[2] = { 0, 0 };
		ReadPlayers(in, false);
		uint16_t pad = in[0] | in[1], pressed = pad & ~lastPad;
		lastPad = pad;
		if (pressed & GUASA_START) paused = !paused;
		if (pressed & GUASA_LEFT) seek(i > kEvery ? i - kEvery : 0);
		if (pressed & GUASA_RIGHT) seek(i + kEvery);
		if (pressed & (GUASA_LP | GUASA_MP | GUASA_HP)) speed = speed == 4 ? 1 : speed * 2;

		if (!shots.empty()) {   // the script
			static int t = 0;
			t++;
			if (t == 1) seek(SDL_min(rep.frames.size() / 2, (size_t)900));
			if (t == 60) SaveWindow(shots + "/repeticion.bmp");
			if (t == 61) { paused = true; showInputs = true; speed = 2; }
			if (t == 70) SaveWindow(shots + "/repeticion-pausa.bmp");
			if (t == 71) quit = true;
		}
		bool done = i >= rep.frames.size();
		if (!paused && !done)
			for (int k = 0; k < speed && i < rep.frames.size(); k++) {
				mark();
				bool moved = ReplayFrame(rep, i, ps, k == speed - 1);
				i++;
				if (moved && speed == 1) PushAudio();   // sound only at normal speed
			}
		uint16_t raw[2] = { 0, 0 };
		if (i > 0) { raw[0] = (uint16_t)(rep.frames[i - 1] & ~kPauseBit); raw[1] = (uint16_t)((rep.frames[i - 1] >> 16) & ~kPauseBit); }
		std::string banner = done ? "FIN DE LA REPETICION\nEsc: salir   Izquierda: 10 s atras" : paused ? "PAUSA\nEspacio o START: seguir" : "";
		g_overlay = [&](const SDL_FRect& r) {
			DrawNames(r);
			float s = r.h / 450 > 1 ? r.h / 450 : 1;
			char line[200];
			snprintf(line, sizeof(line), "%s / %s  x%d   Espacio pausa  F velocidad  <- -> 10 s  F2 botones  Esc",
			         Clock(i).c_str(), Clock(rep.frames.size()).c_str(), speed);
			SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 150);
			SDL_FRect bar{ r.x, r.y, r.w, 14 * s };
			SDL_RenderFillRect(g_ren, &bar);
			Text(r.x + 6 * s, r.y + 3 * s, s, line, 220, 220, 220);
			if (ps.phase) Text(r.x + r.w - 140 * s, r.y + 3 * s, s, "(set en pausa)", 255, 220, 120);
		};
		Present(showInputs ? raw : NULL, banner.empty() ? NULL : banner.c_str());

		next += frameNs;
		Uint64 now = SDL_GetTicksNS();
		if (next > now) SDL_DelayPrecise(next - now);
		else if (now - next > 5 * frameNs) next = now;
	}
	g_overlay = nullptr;
	CloseAll();
	return 0;
}

// espectar: a set as it's played. The frames come on stdin from the app (which
// gets them from the hub, sent by player 1's game); the game here runs them about
// two seconds behind. Joining late (or falling more than 3 seconds behind), it
// catches up hidden, as fast as the PC goes, on a dark screen that says how far
// it is, and then shows the set where it is, at normal speed: never fast motion.
// Keeps the score by counting the games each one wins.
static int Spectate(const char* roms, bool pause, const std::string& shots = "")
{
	char msg[300] = "";
	if (guasa_init(roms, msg, sizeof(msg))) {
		char report[4000] = "";
		guasa_check_rom(roms, report, sizeof(report));
		printf("error: %s\n%s", msg, report);
		return 1;
	}
	guasa_boot_to_select();
	if (!OpenWindow("La Guasabara - Espectar")) return 1;
	LoadMaps();
	printf("espectando\n");
	fflush(stdout);

	static std::mutex lock;
	static Replay live;              // the frames so far
	static std::atomic<bool> over{ false }, leaveNow{ false };   // leaveNow: "salir" (the app stops watching)
	live.pause = pause;
	std::thread([] {
		std::string line;
		while (std::getline(std::cin, line)) {
			if (line.rfind("salir", 0) == 0) { leaveNow = true; return; }
			if (line.rfind("fin", 0) == 0) { over = true; continue; }
			if (line.rfind("cuadros ", 0) != 0) continue;
			size_t sp = line.find(' ', 8);
			if (sp == std::string::npos) continue;
			size_t from = strtoull(line.c_str() + 8, NULL, 10);
			std::vector<uint32_t> got;
			if (!FramesFromBase64(line.substr(sp + 1), got)) continue;
			std::lock_guard<std::mutex> l(lock);
			if (from == live.frames.size()) live.frames.insert(live.frames.end(), got.begin(), got.end());
		}
		over = true;   // the app closed
	}).detach();

	const size_t kBehind = (size_t)(2 * guasa_fps());   // about two seconds behind the players
	const size_t kJump = (size_t)(3 * guasa_fps());     // more than this behind that: catch up hidden
	bool catching = false;
	size_t catchFrom = 0;                                // where the catching up started, for its percentage
	size_t i = 0;
	PauseState ps;
	ResultWatcher results;
	int score[2] = { 0, 0 };
	bool quit = false, showInputs = false;
	Uint64 endedAt = 0;
	const Uint64 frameNs = (Uint64)(1e9 / guasa_fps());
	Uint64 next = SDL_GetTicksNS();
	auto step = [&](bool draw) {
		Replay r;
		{ std::lock_guard<std::mutex> l(lock); r.pause = live.pause; r.frames.assign(1, live.frames[i]); }
		bool moved = ReplayFrame(r, 0, ps, draw);
		i++;
		if (int w = results.Update()) score[w - 1]++;
		return moved;
	};
	while (!quit) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (g_ctl.Event(ev)) continue;
			if (ev.type == SDL_EVENT_QUIT) quit = true;
			if (ev.type == SDL_EVENT_KEY_DOWN) {
				if (ev.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;
				if (ev.key.scancode == SDL_SCANCODE_F2) showInputs = !showInputs;
				if (ev.key.scancode == SDL_SCANCODE_F11 || (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT)))
					SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
			}
		}
		if (leaveNow) quit = true;
		size_t have;
		{ std::lock_guard<std::mutex> l(lock); have = live.frames.size(); }
		bool ended = over;
		size_t target = ended ? have : have > kBehind ? have - kBehind : 0;
		if (!catching && target > i + kJump) { catching = true; catchFrom = i; }
		if (catching) {   // hidden and as fast as it goes, a slice at a time so the window stays alive
			Uint64 until = SDL_GetTicksNS() + 25000000ULL;
			while (i + 1 < target && SDL_GetTicksNS() < until) step(false);
			if (i + 1 >= target) { step(true); catching = false; next = SDL_GetTicksNS(); }   // there: from here, normal speed
		} else if (i < target) {
			if (step(true)) PushAudio();
		}
		std::string banner;
		if (have == 0) banner = "ESPECTAR\nEsperando los primeros cuadros del set...";

		else if (ended && i >= have) {
			if (!endedAt) endedAt = SDL_GetTicksNS();
			banner = "FIN DEL SET\nEsc: salir";
			if (SDL_GetTicksNS() - endedAt > 8000000000ULL) quit = true;
		}
		uint16_t raw[2] = { 0, 0 };
		if (i > 0) { std::lock_guard<std::mutex> l(lock); raw[0] = (uint16_t)(live.frames[i - 1] & ~kPauseBit); raw[1] = (uint16_t)((live.frames[i - 1] >> 16) & ~kPauseBit); }
		g_overlay = [&](const SDL_FRect& r) {
			float s = r.h / 450 > 1 ? r.h / 450 : 1;
			if (catching) {   // dark, with how far it is: the game isn't shown until it's where the set is
				SDL_SetRenderDrawColor(g_ren, 8, 10, 16, 255);
				SDL_RenderFillRect(g_ren, &r);
				int pct = target > catchFrom ? (int)(100.0 * (i - catchFrom) / (target - catchFrom)) : 100;
				char t[64];
				snprintf(t, sizeof(t), "%d%%", pct);
				Text(r.x + r.w / 2 - strlen("CARGANDO SET EN VIVO") * 8 * s, r.y + r.h / 2 - 30 * s, 2 * s, "CARGANDO SET EN VIVO", 255, 46, 136);   // centered
				Text(r.x + r.w / 2 - strlen(t) * 12 * s, r.y + r.h / 2, 3 * s, t, 235, 235, 240);
				SDL_FRect track{ r.x + r.w / 2 - 150 * s, r.y + r.h / 2 + 36 * s, 300 * s, 6 * s }, fill = track;
				fill.w = track.w * pct / 100;
				SDL_SetRenderDrawColor(g_ren, 40, 42, 54, 255);
				SDL_RenderFillRect(g_ren, &track);
				SDL_SetRenderDrawColor(g_ren, 255, 46, 136, 255);
				SDL_RenderFillRect(g_ren, &fill);
				return;
			}
			g_score[0] = score[0];   // the bar shows the score this window counts
			g_score[1] = score[1];
			DrawNames(r);
			char line[160];
			snprintf(line, sizeof(line), "EN VIVO   %d - %d   %s   F2 botones   Esc dejar de ver", score[0], score[1], Clock(i).c_str());
			SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 150);
			SDL_FRect bar{ r.x, r.y, r.w, 14 * s };
			SDL_RenderFillRect(g_ren, &bar);
			Text(r.x + 6 * s, r.y + 3 * s, s, line, 220, 220, 220);
			SDL_SetRenderDrawColor(g_ren, 230, 40, 60, 255);
			SDL_FRect dot{ r.x + r.w - 16 * s, r.y + 3 * s, 8 * s, 8 * s };
			if ((SDL_GetTicks() / 600) % 2 && !ended) SDL_RenderFillRect(g_ren, &dot);
			if (ps.phase) Text(r.x + r.w - 150 * s, r.y + 3 * s, s, "(set en pausa)", 255, 220, 120);
		};
		Present(showInputs ? raw : NULL, banner.empty() ? NULL : banner.c_str());
		static bool shot = false;   // captura=<folder>: one picture once it plays live
		static size_t liveAt = 0;   // the frame it got to the set live (the picture, a second after)
		if (!catching && !liveAt && i > 900) liveAt = i;
		if (!shots.empty() && !shot && liveAt && i > liveAt + 60) { SaveWindow(shots + "/espectar.bmp"); shot = true; printf("en vivo en el cuadro %zu de %zu\n", i, have); fflush(stdout); }
		static bool shotCatch = false;   // and one while it catches up
		if (!shots.empty() && !shotCatch && catching && i > catchFrom + 1500) { SaveWindow(shots + "/espectar-llegando.bmp"); shotCatch = true; }
		next += frameNs;
		Uint64 now = SDL_GetTicksNS();
		if (next > now) SDL_DelayPrecise(next - now);
		else if (now - next > 5 * frameNs) next = now;
	}
	printf("espectado: %zu cuadros, estado final %016llx\n", i, (unsigned long long)guasa_state_hash());
	fflush(stdout);
	g_overlay = nullptr;
	CloseAll();
	return 0;
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
	std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "controles") {   // no ROMs needed
		std::string shots;
		for (int i = 2; i < argc; i++) {
			if (!strncmp(argv[i], "captura=", 8)) shots = argv[i] + 8;
			if (!strncmp(argv[i], "archivo=", 8)) g_mapsFile = argv[i] + 8;
		}
		return ControlsScreen(shots);
	}
	if (argc < 3) {
		fprintf(stderr, "uso: guasa-game check|versus|test|online <carpeta de ROMs> ...\n");
		return 2;
	}
	if (mode == "check") {
		char report[4000] = "";
		int n = guasa_check_rom(argv[2], report, sizeof(report));
		printf("%s", report);
		return n == 0 ? 0 : 1;
	}
	if (mode == "versus" || mode == "test" || mode == "arcade") {
		std::string shots;
		for (int i = 3; i < argc; i++)
			if (!strncmp(argv[i], "captura=", 8)) shots = argv[i] + 8;
		if (mode == "arcade") return PlayArcade(argv[2], shots);
		return Play(argv[2], mode == "test", shots);
	}
	if (mode == "practica") {
		std::string shots;
		int stage = 0x0C;   // Cammy's, unless the app says otherwise
		for (int i = 3; i < argc; i++) {
			if (!strncmp(argv[i], "captura=", 8)) shots = argv[i] + 8;
			if (!strcmp(argv[i], "escenario=practica")) stage = kOwnStage;
			if (!strcmp(argv[i], "escenario=juego")) stage = -1;
		}
		return PlayPractice(argv[2], shots, stage);
	}
	if (mode == "practica-prueba") return PracticeSelfTest(argv[2]);
	if (mode == "sonido-reto" && argc >= 4) return ChallengerSound(argv[2], argv[3]);
	if (mode == "espectar") {
		bool pause = false;
		std::string shots;
		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "pausa")) pause = true;
			if (!strncmp(argv[i], "captura=", 8)) shots = argv[i] + 8;
			if ((!strncmp(argv[i], "j1=", 3) || !strncmp(argv[i], "j2=", 3)) && strchr(argv[i], ':')) {
				int p = argv[i][1] - '1';
				const char* colon = strchr(argv[i], ':');
				std::string rank(argv[i] + 3, (size_t)(colon - (argv[i] + 3)));
				g_ranks[p] = rank == "-" ? "" : rank;
				g_names[p] = colon + 1;
			}
		}
		return Spectate(argv[2], pause, shots);
	}
	if (mode == "repeticion" || mode == "repeticion-prueba") {
		if (argc < 4) { fprintf(stderr, "uso: guasa-game %s <carpeta de ROMs> <archivo>\n", mode.c_str()); return 2; }
		for (int i = 4; i < argc; i++)   // j1=<rank|->:<name>, j2=...: the names on top
			if ((!strncmp(argv[i], "j1=", 3) || !strncmp(argv[i], "j2=", 3)) && strchr(argv[i], ':')) {
				int p = argv[i][1] - '1';
				const char* colon = strchr(argv[i], ':');
				std::string rank(argv[i] + 3, (size_t)(colon - (argv[i] + 3)));
				g_ranks[p] = rank == "-" ? "" : rank;
				g_names[p] = colon + 1;
			}
		std::string shots;
		for (int i = 4; i < argc; i++) if (!strncmp(argv[i], "captura=", 8)) shots = argv[i] + 8;
		return mode == "repeticion" ? PlayReplay(argv[2], argv[3], shots) : ReplayCheck(argv[2], argv[3]);
	}
	if (mode == "online") {
		NetConfig cfg;
		if (argc >= 5) {
			cfg.local = atoi(argv[3]) - 1;
			cfg.port = atoi(argv[4]);
		}
		std::string shots, recordTo;
		bool stream = false;
		for (int i = 5; i < argc; i++)
			if (!strncmp(argv[i], "captura=", 8)) shots = argv[i] + 8;
			else if (!strncmp(argv[i], "grabar=", 7)) recordTo = argv[i] + 7;
			else if (!strcmp(argv[i], "transmitir")) stream = true;
			else if (!strcmp(argv[i], "sinx3")) g_noX3 = true;   // someone in this set turns x3 down
			else if (!ParseNetArg(argv[i], cfg)) { fprintf(stderr, "opción desconocida: %s\n", argv[i]); return 2; }
		if (argc < 6 || cfg.local < 0 || cfg.local > 1 || (cfg.remote.empty() && (cfg.relay.empty() || cfg.token.size() != 16))) {
			fprintf(stderr, "uso: guasa-game online <carpeta de ROMs> <1|2> <puerto_local> (<ip:puerto> | relay=<host:puerto> token=<32 hex> [relayonly]) [delay=N|auto] [lag=MS] [jitter=MS] [loss=PCT]\n");
			return 2;
		}
		return PlayOnline(argv[2], cfg, shots, recordTo, stream);
	}
	fprintf(stderr, "modo desconocido: %s\n", mode.c_str());
	return 2;
}
