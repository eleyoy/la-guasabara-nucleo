// guasa-core, level 1: the pinned fightcade-fbneo CPS2 core behind guasa_core.h.
// The front-end values below follow a Fightcade netplay session
// (burner/win32/fbn_ggpo.cpp and main.cpp in the fork); see spike/RESULTADOS.md.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <tchar.h>   // after stdio.h: the SDL port's tchar.h redefines dprintf
#include "burn.h"
#include "zlib.h"
#include "guasa_core.h"

// ---------------------------------------------------------------------------
// Front-end symbols the fork's core expects from Fightcade (burner/win32).

int kNetGame = 1;               // netplay: fixed BurnRandom seed and fixed clock (burn.cpp)
int kNetSpectator = 0;
int kNetVersion = NET_VERSION;  // 14: CPS RW scan in the state (cps_mem.cpp)
INT32 is_netgame_or_recording() { return kNetGame; }
#if defined(BUILD_WIN32)        // the SDL build defines it in burn.cpp
struct MovieExtInfo { UINT32 year, month, day, hour, minute, second; };
struct MovieExtInfo MovieInfo = { 0, 0, 0, 0, 0, 0 };
#endif
int bDrvOkay = 1;
bool bDoIpsPatch = false;       // Fightcade's only IPS for this game (Auto-Reversal) is off in matches
TCHAR szAppHiscorePath[MAX_PATH] = _T("");   // no hiscore.dat: high scores stay off
TCHAR szAppBlendPath[MAX_PATH] = _T("");
void IpsApplyPatches(UINT8*, char*) {}
void Reinitialise() {}
// cheat.cpp watch mode and Lua memory hooks: not used (no cheats or Lua in ranked).
INT32 VidSNewTinyMsg(const TCHAR*, INT32, INT32, INT32) { return 0; }
enum LuaMemHookType : int;
void CallRegisteredLuaMemHook(unsigned int, int, unsigned int, LuaMemHookType) {}

void QscScanResampler(INT32 nAction);   // spike/patches/0001
extern UINT8* CpsRamFF;                 // 68000 work RAM, 0xFF0000-0xFFFFFF

// ---------------------------------------------------------------------------
// XXH64 (seed 0), from the public xxHash specification.

static const uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL,
	P3 = 1609587929392839161ULL, P4 = 9650029242287828579ULL, P5 = 2870177450012600261ULL;

static inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t Read64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }   // little-endian x86
static inline uint32_t Read32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t Round(uint64_t acc, uint64_t in) { acc += in * P2; return Rotl(acc, 31) * P1; }
static inline uint64_t Merge(uint64_t acc, uint64_t v) { acc ^= Round(0, v); return acc * P1 + P4; }

uint64_t guasa_xxh64(const void* data, size_t len)
{
	const uint8_t* p = (const uint8_t*)data;
	const uint8_t* end = p + len;
	uint64_t h;
	if (len >= 32) {
		uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
		for (; p + 32 <= end; p += 32) {
			v1 = Round(v1, Read64(p)); v2 = Round(v2, Read64(p + 8));
			v3 = Round(v3, Read64(p + 16)); v4 = Round(v4, Read64(p + 24));
		}
		h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
		h = Merge(h, v1); h = Merge(h, v2); h = Merge(h, v3); h = Merge(h, v4);
	} else {
		h = P5;
	}
	h += len;
	for (; p + 8 <= end; p += 8) { h ^= Round(0, Read64(p)); h = Rotl(h, 27) * P1 + P4; }
	if (p + 4 <= end) { h ^= (uint64_t)Read32(p) * P1; h = Rotl(h, 23) * P2 + P3; p += 4; }
	for (; p < end; p++) { h ^= *p * P5; h = Rotl(h, 11) * P1; }
	h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
	return h;
}

// ---------------------------------------------------------------------------
// ROMs, matched by CRC32 so names and folders don't matter: loose files or the
// entries of any .zip in the folder. A zip already lists each entry's CRC in its
// central directory, so finding the files doesn't need to decompress anything.

namespace fs = std::filesystem;

struct RomSource {
	fs::path file;
	bool zipped;
	UINT16 method;            // zip: 0 stored, 8 deflate
	UINT32 local, csize, usize;
};
static std::map<UINT32, RomSource> g_romByCrc;

static UINT32 Crc32(const UINT8* p, size_t n)
{
	UINT32 c = 0xFFFFFFFF;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
	}
	return ~c;
}

static bool ReadBytes(const fs::path& path, size_t at, size_t n, std::vector<UINT8>& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	f.seekg(at);
	out.resize(n);
	f.read((char*)out.data(), n);
	return (size_t)f.gcount() == n;
}

static UINT16 Le16(const UINT8* p) { return p[0] | (p[1] << 8); }
static UINT32 Le32(const UINT8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((UINT32)p[3] << 24); }

struct ZipEntry { std::string name; UINT32 crc, usize; RomSource src; };

// The entries of a zip, from its central directory.
static std::vector<ZipEntry> ListZip(const fs::path& path)
{
	std::vector<ZipEntry> out;
	std::error_code ec;
	size_t size = (size_t)fs::file_size(path, ec);
	if (ec || size < 22) return out;
	size_t tail = size < 65557 ? size : 65557;   // end record: 22 bytes plus a comment of up to 64 KB
	std::vector<UINT8> t;
	if (!ReadBytes(path, size - tail, tail, t)) return out;
	size_t eocd = std::string::npos;
	for (size_t i = tail - 22 + 1; i-- > 0;)
		if (Le32(&t[i]) == 0x06054b50) { eocd = i; break; }
	if (eocd == std::string::npos) return out;
	UINT32 count = Le16(&t[eocd + 10]), cdSize = Le32(&t[eocd + 12]), cdAt = Le32(&t[eocd + 16]);
	std::vector<UINT8> cd;
	if (!ReadBytes(path, cdAt, cdSize, cd)) return out;
	for (size_t p = 0; count-- && p + 46 <= cd.size() && Le32(&cd[p]) == 0x02014b50;) {
		UINT16 nameLen = Le16(&cd[p + 28]), extraLen = Le16(&cd[p + 30]), commentLen = Le16(&cd[p + 32]);
		if (p + 46 + nameLen > cd.size()) break;
		ZipEntry e;
		e.name.assign((const char*)&cd[p + 46], nameLen);
		e.crc = Le32(&cd[p + 16]);
		e.usize = Le32(&cd[p + 24]);
		e.src = { path, true, Le16(&cd[p + 10]), Le32(&cd[p + 42]), Le32(&cd[p + 20]), e.usize };
		size_t slash = e.name.find_last_of("/\\");   // files inside a folder in the zip count too
		if (slash != std::string::npos) e.name = e.name.substr(slash + 1);
		out.push_back(e);
		p += 46 + nameLen + extraLen + commentLen;
	}
	return out;
}

static bool ReadRom(const RomSource& s, std::vector<UINT8>& out)
{
	if (!s.zipped) {
		std::error_code ec;
		size_t n = (size_t)fs::file_size(s.file, ec);
		return !ec && ReadBytes(s.file, 0, n, out);
	}
	std::vector<UINT8> h;
	if (!ReadBytes(s.file, s.local, 30, h) || Le32(&h[0]) != 0x04034b50) return false;
	std::vector<UINT8> packed;
	if (!ReadBytes(s.file, s.local + 30 + Le16(&h[26]) + Le16(&h[28]), s.csize, packed)) return false;
	if (s.method == 0) { out = packed; return out.size() == s.usize; }
	if (s.method != 8) return false;
	out.assign(s.usize, 0);
	z_stream z = {};
	if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;   // raw deflate, as zip stores it
	z.next_in = packed.data();
	z.avail_in = (uInt)packed.size();
	z.next_out = out.data();
	z.avail_out = (uInt)out.size();
	int r = inflate(&z, Z_FINISH);
	inflateEnd(&z);
	return r == Z_STREAM_END && z.total_out == s.usize;
}

// The files the driver asks for (name, size and CRC), without the "no dump" ones.
static std::vector<BurnRomInfo> WantedRoms(std::vector<std::string>* names)
{
	std::vector<BurnRomInfo> out;
	BurnRomInfo ri;
	for (UINT32 i = 0; BurnDrvGetRomInfo(&ri, i) == 0; i++) {
		if (ri.nLen == 0 || (ri.nType & BRF_NODUMP)) continue;
		char* name = NULL;
		BurnDrvGetRomName(&name, i, 0);
		out.push_back(ri);
		if (names) names->push_back(name ? name : "?");
	}
	return out;
}

// Looks through rom_dir for the files the driver wants. Loose files are only read
// when their size matches one of them, so a big ROM folder (Fightcade's) is quick.
// Every file in dir and its subfolders, up to 3 levels down: unzipping often
// leaves the files in a folder inside another one. A limit on how many it looks
// at keeps a whole disk from being walked by mistake.
template <class F> static bool ForEachFile(const fs::path& dir, F fn)
{
	std::error_code ec;
	fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
	if (ec) return false;
	int seen = 0;
	for (; it != end && seen < 20000; it.increment(ec), seen++) {
		if (ec) { ec.clear(); continue; }
		if (it->is_directory(ec) && it.depth() >= 3) it.disable_recursion_pending();
		if (it->is_regular_file(ec)) fn(*it);
	}
	return true;
}

static void IndexRoms(const fs::path& dir)
{
	std::map<UINT32, UINT32> sizeByCrc;
	std::vector<std::string> names;
	for (auto& r : WantedRoms(&names)) sizeByCrc[r.nCrc] = r.nLen;
	auto look = [&](const fs::directory_entry& e) {
		std::error_code ec;
		std::string ext = e.path().extension().string();
		for (auto& c : ext) c = (char)tolower(c);
		if (ext == ".zip") {
			for (auto& z : ListZip(e.path()))
				if (sizeByCrc.count(z.crc) && sizeByCrc[z.crc] == z.usize) g_romByCrc.emplace(z.crc, z.src);
			return;
		}
		size_t n = (size_t)e.file_size(ec);
		bool wanted = false;
		for (auto& w : sizeByCrc) wanted = wanted || w.second == n;
		std::vector<UINT8> d;
		if (wanted && ReadBytes(e.path(), 0, n, d)) {
			UINT32 crc = Crc32(d.data(), d.size());
			if (sizeByCrc.count(crc)) g_romByCrc.emplace(crc, RomSource{ e.path(), false, 0, 0, 0, (UINT32)n });
		}
	};
	// First the files the game comes in: a whole collection (Fightcade's ROMs
	// folder has thousands of zips, maybe on a slow disk) isn't read if those
	// bring everything.
	std::vector<std::string> first = { "ssf2xjr1.zip", "ssf2t.zip" };
	first.insert(first.end(), names.begin(), names.end());
	for (auto& n : first) {
		std::error_code ec;
		fs::directory_entry e(dir / n, ec);
		if (!ec && e.is_regular_file(ec)) look(e);
	}
	if (g_romByCrc.size() < sizeByCrc.size()) ForEachFile(dir, look);
}

static std::string g_missingRom;

static INT32 __cdecl LoadRom(UINT8* dest, INT32* wrote, INT32 i)
{
	BurnRomInfo ri;
	if (BurnDrvGetRomInfo(&ri, i)) return 1;
	auto it = g_romByCrc.find(ri.nCrc);
	std::vector<UINT8> d;
	if (it == g_romByCrc.end() || !ReadRom(it->second, d)) {
		char* name = NULL;
		BurnDrvGetRomName(&name, i, 0);
		g_missingRom = name ? name : "?";
		return 1;
	}
	if (dest) memcpy(dest, d.data(), d.size() < ri.nLen ? d.size() : ri.nLen);
	if (wrote) *wrote = (INT32)d.size();
	return 0;
}

static UINT32 __cdecl HighCol16(INT32 r, INT32 g, INT32 b, INT32)
{
	return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static INT32 __cdecl Quiet(INT32, TCHAR*, ...) { return 0; }

// ---------------------------------------------------------------------------
// State: the scan Fightcade uses for netplay (ACB_FULLSCANL) plus the QSound
// resampler state that scan leaves out.

static std::vector<UINT8> g_state;
static size_t g_statePos;
static UINT8* g_loadSrc;

static INT32 __cdecl AcbSave(BurnArea* pba)
{
	g_state.insert(g_state.end(), (UINT8*)pba->Data, (UINT8*)pba->Data + pba->nLen);
	return 0;
}

static INT32 __cdecl AcbLoad(BurnArea* pba)
{
	memcpy(pba->Data, g_loadSrc + g_statePos, pba->nLen);
	g_statePos += pba->nLen;
	return 0;
}

static const std::vector<UINT8>& Snapshot()
{
	g_state.clear();
	BurnAcb = AcbSave;
	BurnAreaScan(ACB_FULLSCANL | ACB_READ, NULL);
	QscScanResampler(ACB_DRIVER_DATA | ACB_READ);
	return g_state;
}

// ---------------------------------------------------------------------------

static bool g_ready;
static std::vector<INT16> g_audio;
static std::vector<UINT8> g_frame;
static INT32 g_w, g_h;
static UINT8* g_pad[2][12];
static size_t g_stateSize;

static const char* kPadNames[12] = { "Up", "Down", "Left", "Right", "Weak Punch", "Medium Punch",
	"Strong Punch", "Weak Kick", "Medium Kick", "Strong Kick", "Start", "Coin" };

static void FindPads()
{
	BurnInputInfo bii;
	for (UINT32 i = 0; BurnDrvGetInputInfo(&bii, i) == 0; i++)
		for (int p = 0; p < 2; p++)
			for (int k = 0; k < 12; k++) {
				char name[32];
				snprintf(name, sizeof(name), "P%d %s", p + 1, kPadNames[k]);
				if (bii.szName && strcmp(bii.szName, name) == 0) g_pad[p][k] = bii.pVal;
			}
}

// The league's SOCD rule: left + right = neutral, up + down = up (Fightcade's
// "Hitbox" mode). The core's own StopOpposite() runs after this and finds
// nothing left to clean.
static uint16_t CleanSocd(uint16_t in)
{
	if ((in & (GUASA_LEFT | GUASA_RIGHT)) == (GUASA_LEFT | GUASA_RIGHT)) in &= ~(GUASA_LEFT | GUASA_RIGHT);
	if ((in & (GUASA_UP | GUASA_DOWN)) == (GUASA_UP | GUASA_DOWN)) in &= ~GUASA_DOWN;
	return in;
}

uint16_t guasa_socd(uint16_t input) { return CleanSocd(input); }

static void Fail(char* message, size_t len, const std::string& why)
{
	if (message && len) snprintf(message, len, "%s", why.c_str());
}

static bool g_libReady;

// Starts the library once and makes ssf2xjr1 the active driver.
static bool SelectDriver()
{
	if (!g_libReady) {
		bprintf = Quiet;
		BurnExtLoadRom = LoadRom;
		BurnLibInit();
		g_libReady = true;
	}
	for (UINT32 i = 0; i < nBurnDrvCount; i++) {
		nBurnDrvActive = i;
		if (strcmp(BurnDrvGetTextA(DRV_NAME), "ssf2xjr1") == 0) {
			nBurnDrvSelect[0] = i;
			return true;
		}
	}
	return false;
}

int guasa_init(const char* rom_dir, char* message, size_t message_len)
{
	if (g_ready) { Fail(message, message_len, "already started: one game per process"); return 1; }
	if (!SelectDriver()) { Fail(message, message_len, "driver ssf2xjr1 not found"); return 1; }
	g_romByCrc.clear();
	IndexRoms(fs::u8path(rom_dir));

	nBurnSoundRate = GUASA_SOUND_RATE;
	nBurnSoundLen = (nBurnSoundRate * 100 + nBurnFPS / 2) / nBurnFPS;
	g_audio.assign(nBurnSoundLen * 2 * 2, 0);
	pBurnSoundOut = g_audio.data();     // rule 1: QSound always rendered
	nBurnCPUSpeedAdjust = 0x0100;       // CPU at 100%, as in Fightcade netplay
	nBurnBpp = 2;
	BurnHighCol = HighCol16;

	if (BurnDrvInit()) {
		Fail(message, message_len, g_missingRom.empty() ? "the core didn't start" : "missing or wrong file: " + g_missingRom);
		return 1;
	}
	BurnDrvGetFullSize(&g_w, &g_h);
	g_frame.assign(g_w * g_h * 2, 0);
	nBurnPitch = g_w * 2;
	FindPads();
	g_stateSize = Snapshot().size();
	g_ready = true;
	return 0;
}

int guasa_check_rom(const char* rom_dir, char* report, size_t report_len)
{
	std::string out;
	if (g_ready) { Fail(report, report_len, "the game is running"); return -1; }
	if (!SelectDriver()) { Fail(report, report_len, "driver ssf2xjr1 not found"); return -1; }
	std::vector<std::string> names;
	std::vector<BurnRomInfo> wanted = WantedRoms(&names);

	// What the folder has, by CRC (any name) and by name (any CRC).
	g_romByCrc.clear();
	fs::path dir = fs::u8path(rom_dir);
	IndexRoms(dir);
	std::map<std::string, UINT32> crcByName;   // files with the right name, in zips or loose
	std::map<std::string, bool> isWanted;
	for (auto& n : names) isWanted[n] = true;
	std::map<UINT32, bool> crcs;
	for (auto& w : wanted) crcs[w.nCrc] = true;
	bool complete = g_romByCrc.size() == crcs.size();   // then the names aren't needed
	bool opened = complete || ForEachFile(dir, [&](const fs::directory_entry& e) {
		std::string ext = e.path().extension().string();
		for (auto& c : ext) c = (char)tolower(c);
		if (ext == ".zip") { for (auto& z : ListZip(e.path())) crcByName.emplace(z.name, z.crc); return; }
		std::string name = e.path().filename().string();
		std::vector<UINT8> d;
		std::error_code ec;
		if (isWanted.count(name) && ReadBytes(e.path(), 0, (size_t)e.file_size(ec), d)) crcByName.emplace(name, Crc32(d.data(), d.size()));
	});
	if (!opened) { Fail(report, report_len, "No se pudo abrir la carpeta."); return -1; }

	// The game's own files are sfxj.* and ssf2xj.key (ssf2xjr1.zip); the rest are its
	// parent's, ssf2t.zip (graphics and sound). Fightcade's ssf2xjr1.zip brings both.
	int problems = 0, ownMissing = 0, ownTotal = 0, parentMissing = 0, wrong = 0;
	std::string lines;
	for (size_t i = 0; i < wanted.size(); i++) {
		const std::string& n = names[i];
		bool own = n.rfind("sfxj", 0) == 0 || n.rfind("ssf2xj", 0) == 0;
		ownTotal += own;
		if (g_romByCrc.count(wanted[i].nCrc)) continue;
		problems++;
		char line[200];
		auto found = crcByName.find(n);
		if (found != crcByName.end()) {
			wrong++;
			snprintf(line, sizeof(line), "%s está, pero no es el correcto (CRC %08x, se espera %08x).\n",
			         n.c_str(), found->second, wanted[i].nCrc);
		} else {
			(own ? ownMissing : parentMissing)++;
			snprintf(line, sizeof(line), "Falta %s (va en %s).\n", n.c_str(), own ? "ssf2xjr1.zip" : "ssf2t.zip o en ssf2xjr1.zip");
		}
		lines += line;
	}
	int parentTotal = (int)wanted.size() - ownTotal;
	bool none = g_romByCrc.empty();
	g_romByCrc.clear();
	if (!problems) out = "Todo bien: están los " + std::to_string(wanted.size()) + " archivos del juego.\n";
	else if (none && !wrong)   // nothing of the game here: one line instead of the whole list
		out = "En esta carpeta no está el juego. Busca ssf2xjr1.zip (y ssf2t.zip, si lo tienes aparte).\n";
	else if (!wrong && !ownMissing && parentMissing == parentTotal)
		out = "Encontré ssf2xjr1.zip, pero a ese juego le falta ssf2t.zip, que trae los gráficos y el sonido. "
		      "Pon ssf2t.zip en la misma carpeta, o usa el ssf2xjr1.zip de Fightcade, que ya trae todo.\n";
	else if (!wrong && !parentMissing && ownMissing == ownTotal)
		out = "Encontré ssf2t.zip, pero falta ssf2xjr1.zip, que es el juego en sí. Ponlo en la misma carpeta.\n";
	else
		out = lines;
	Fail(report, report_len, out);
	return problems;
}

void guasa_exit(void)
{
	if (!g_ready) return;
	BurnDrvExit();
	BurnLibExit();
	g_ready = false;
}

double guasa_fps(void) { return nBurnFPS / 100.0; }

void guasa_run_frame(const uint16_t inputs[2], int draw)
{
	for (int p = 0; p < 2; p++) {
		uint16_t v = CleanSocd(inputs[p]);
		for (int k = 0; k < 12; k++)
			if (g_pad[p][k]) *g_pad[p][k] = (v >> k) & 1;
	}
	pBurnDraw = draw ? g_frame.data() : NULL;
	BurnDrvFrame();
}

size_t guasa_state_size(void) { return g_stateSize; }

size_t guasa_save(uint8_t* buf, size_t cap)
{
	const std::vector<UINT8>& s = Snapshot();
	if (s.size() > cap) return 0;
	memcpy(buf, s.data(), s.size());
	return s.size();
}

int guasa_load(const uint8_t* buf, size_t len)
{
	if (len != g_stateSize) return 1;
	g_loadSrc = (UINT8*)buf;
	g_statePos = 0;
	BurnAcb = AcbLoad;
	BurnAreaScan(ACB_FULLSCANL | ACB_WRITE, NULL);
	QscScanResampler(ACB_DRIVER_DATA | ACB_WRITE);
	return g_statePos == len ? 0 : 1;
}

int guasa_load_state_file(const uint8_t* buf, size_t len)
{
	g_state.clear();
	BurnAcb = AcbSave;
	BurnAreaScan(ACB_FULLSCAN | ACB_READ, NULL);   // only to learn the size
	if (len != g_state.size()) return 1;
	g_loadSrc = (UINT8*)buf;
	g_statePos = 0;
	BurnAcb = AcbLoad;
	BurnAreaScan(ACB_FULLSCAN | ACB_WRITE, NULL);   // as BurnStateLoad does (burner/statec.cpp)
	return g_statePos == len ? 0 : 1;
}

uint64_t guasa_state_hash(void)
{
	const std::vector<UINT8>& s = Snapshot();
	return guasa_xxh64(s.data(), s.size());
}

uint64_t guasa_game_hash(void) { return guasa_xxh64(CpsRamFF, 0x10000); }

uint8_t guasa_read_ram(uint32_t address)
{
	return CpsRamFF[(address & 0xFFFF) ^ 1];   // stored as little-endian 16-bit words
}

void guasa_write_ram(uint32_t address, uint8_t value)
{
	CpsRamFF[(address & 0xFFFF) ^ 1] = value;
}

const uint16_t* guasa_framebuffer(int* width, int* height, int* pitch)
{
	if (width) *width = g_w;
	if (height) *height = g_h;
	if (pitch) *pitch = nBurnPitch;
	return (const uint16_t*)g_frame.data();
}

const int16_t* guasa_audio(int* samples)
{
	if (samples) *samples = nBurnSoundLen;
	return pBurnSoundOut;
}

// Coins and starts for both players at fixed frames, past the CPS2 boot (coins
// before frame ~800 are ignored) and checked on screen with "core_test select".
// Each player puts in kCoins coins, then P1 starts and, a little later, P2.
// 18 coins minus the two starts leaves 16 credits (FF82E9 = 0x16, BCD), as in
// the state Fightcade loads, so players can continue without coins. Any change
// here changes every player's starting state.
static const int kFirstCoin = 1000, kCoinGap = 20, kCoins = 9;
static const int kP1Start = kFirstCoin + kCoins * kCoinGap + 60, kP2Start = kP1Start + 140;
static const int kSelectFrame = kP2Start + 300;

static bool Pulse(int f, int at) { return f >= at && f < at + 6; }   // a 6-frame press

int guasa_boot_to_select(void)
{
	if (!g_ready) return -1;
	for (int f = 0; f < kSelectFrame; f++) {
		uint16_t in[2] = { 0, 0 };
		for (int c = 0; c < kCoins; c++) {
			if (Pulse(f, kFirstCoin + c * kCoinGap)) in[0] |= GUASA_COIN;
			if (Pulse(f, kFirstCoin + c * kCoinGap + kCoinGap / 2)) in[1] |= GUASA_COIN;
		}
		if (Pulse(f, kP1Start)) in[0] |= GUASA_START;
		if (Pulse(f, kP2Start)) in[1] |= GUASA_START;
		guasa_run_frame(in, 0);
	}
	return kSelectFrame;
}
