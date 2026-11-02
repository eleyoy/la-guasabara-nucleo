// Tests for guasa-core. One game per process, so each mode is one run:
//
//   core_test <roms> check              XXH64 vectors, boot to select, hashes, speed
//   core_test <roms> romcheck           what guasa_check_rom says about the folder
//   core_test <roms> select <out.bmp>   picture of the starting state
//   core_test <roms> hashes <frames>    boot to select, then random inputs: one hash per frame
//   core_test <roms> rollback <frames>  save, load and 7-frame rollback on every frame
//   core_test <roms> state <out.raw>    the starting state, for comparisons
//   core_test <roms> cert <inputs.txt> <out.bin> [state=<raw>] [bmp=<path>]   RAM after every frame (core/cert/)
//   core_test <roms> frames <script.txt> <out.bin>   scripted fight scenarios, RAM after every frame (frame meter research)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <chrono>
#include "guasa_core.h"

static uint64_t g_rng = 12345;
static uint16_t Mash()
{
	uint16_t in = 0;
	for (int k = 0; k < 10; k++) {
		g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
		if ((uint32_t)(g_rng >> 33) % 100 < 30) in |= 1 << k;
	}
	return in;
}

static void Inputs(uint16_t in[2]) { in[0] = Mash(); in[1] = Mash(); }

static int SaveBmp(const char* path)
{
	int w, h, pitch;
	const uint16_t* px = guasa_framebuffer(&w, &h, &pitch);
	FILE* fp = fopen(path, "wb");
	if (!fp) return 1;
	int row = (w * 3 + 3) & ~3;
	uint8_t hdr[54] = { 'B', 'M' };
	uint32_t size = 54 + row * h, off = 54, dib = 40;
	int32_t ww = w, hh = h;
	uint16_t planes = 1, bpp = 24;
	memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
	memcpy(hdr + 18, &ww, 4); memcpy(hdr + 22, &hh, 4); memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
	fwrite(hdr, 1, 54, fp);
	std::vector<uint8_t> line(row, 0);
	for (int y = h - 1; y >= 0; y--) {
		const uint16_t* src = (const uint16_t*)((const uint8_t*)px + y * pitch);
		for (int x = 0; x < w; x++) {
			uint16_t c = src[x];
			line[x * 3 + 0] = (uint8_t)((c & 0x1F) << 3);
			line[x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2);
			line[x * 3 + 2] = (uint8_t)((c >> 11) << 3);
		}
		fwrite(line.data(), 1, row, fp);
	}
	fclose(fp);
	return 0;
}

static bool CheckXxh64()
{
	// Published XXH64 test values (seed 0).
	struct { const char* s; uint64_t h; } v[] = {
		{ "", 0xEF46DB3751D8E999ULL },
		{ "a", 0xD24EC4F1A98C6E5BULL },
		{ "abc", 0x44BC2CF5AD770999ULL },
		{ "Nobody inspects the spammish repetition", 0xFBCEA83C8A378BF1ULL },
	};
	bool ok = true;
	for (auto& t : v) {
		uint64_t h = guasa_xxh64(t.s, strlen(t.s));
		if (h != t.h) { printf("  XXH64(\"%s\") = %016llx, expected %016llx\n", t.s, (unsigned long long)h, (unsigned long long)t.h); ok = false; }
	}
	return ok;
}

// From character select: moves each cursor (L R U D per step), picks with LP,
// and runs until the clock first ticks (99 -> 98). Returns the frames waited.
static int PickAndFight(const std::string moves[2], int draw)
{
	uint16_t in[2] = { 0, 0 };
	auto run = [&](int frames, uint16_t p1, uint16_t p2) { for (int f = 0; f < frames; f++) { in[0] = p1; in[1] = p2; guasa_run_frame(in, draw); } };
	auto dir = [](const std::string& m, size_t k) -> uint16_t {
		if (k >= m.size()) return 0;
		char c = m[k];
		return c == 'L' ? GUASA_LEFT : c == 'R' ? GUASA_RIGHT : c == 'U' ? GUASA_UP : c == 'D' ? GUASA_DOWN : 0;
	};
	run(30, 0, 0);
	for (size_t k = 0; k < moves[0].size() || k < moves[1].size(); k++) { run(4, dir(moves[0], k), dir(moves[1], k)); run(8, 0, 0); }
	run(4, GUASA_LP, GUASA_LP); run(30, 0, 0); run(4, GUASA_LP, GUASA_LP);
	int f = 0;
	for (; f < 4000 && guasa_read_ram(0xFF8DCE) != 0x99; f++) run(1, 0, 0);
	for (; f < 8000 && guasa_read_ram(0xFF8DCE) != 0x98; f++) run(1, 0, 0);
	return f;
}

// Hitbox viewer research (mode "boxes"). The recipe, after the mame-rr script
// sf2-hitboxes.lua by Dammit / felineki (ssf2t profile, as kept in
// github.com/peon2/fbneo-training-mode hitboxes/), checked against our frames:
//   camera: screen_left = s16 FF8ED4, screen_top = s16 FF8ED8
//   object: X = s16 +06, Y = s16 +0A, flip = u8 +12 (1 = facing left),
//           anim frame = u32 +1A, box table = u32 +34 (ROM)
//   screen: sx = X - screen_left, sy = 224 - (Y - 15) + screen_top (the feet)
//   box kind k: id = u8 anim[k.id_at]; 0 = none; table = boxtab + s16 boxtab[k.tab];
//           entry = table + id * k.size: s8 dx, s8 dy, u8 rx, u8 ry
//           (attack: if u8 entry[5] >= 0x80, dx = -entry[5]);
//           centre (sx + (flip ? -dx : dx), sy - dy), half sizes rx, ry
struct HitBox { int obj; std::string type; int id, l, t, r, b; };

static uint16_t MemW(uint32_t a) { return (uint16_t)(guasa_read_mem(a) << 8 | guasa_read_mem(a + 1)); }
static uint32_t MemL(uint32_t a) { return (uint32_t)MemW(a) << 16 | MemW(a + 2); }

static std::vector<HitBox> ComputeBoxes()
{
	struct Kind { int tab, idAt, size; const char* type; };
	static const Kind kinds[] = {
		{ 0x8, 0xD, 0x04, "push" },
		{ 0x0, 0x8, 0x04, "hurt" },
		{ 0x2, 0x9, 0x04, "hurt" },
		{ 0x4, 0xA, 0x04, "hurt" },
		{ 0x6, 0xC, 0x10, "attack" },
	};
	std::vector<HitBox> out;
	int left = (int16_t)MemW(0xFF8ED4), top = (int16_t)MemW(0xFF8ED8);
	auto object = [&](int obj, uint32_t base, bool proj) {
		int sx = (int16_t)MemW(base + 0x06) - left;
		int sy = 224 - ((int16_t)MemW(base + 0x0A) - 15) + top;
		bool flip = guasa_read_mem(base + 0x12) == 1;
		uint32_t anim = MemL(base + 0x1A), tab = MemL(base + 0x34);
		out.push_back({ obj, "axis", 0, sx, sy, sx, sy });
		for (auto& k : kinds) {
			int id = guasa_read_mem(anim + k.idAt);
			if (!id) continue;
			std::string type = k.type;
			if (proj) { if (type == "attack") type = "proj_attack"; else if (type == "push") type = "proj_hurt"; }
			else if (type == "attack" && guasa_read_mem(base + 0x03) == 0) continue;
			uint32_t e = tab + (int16_t)MemW(tab + k.tab) + id * k.size;
			int dx = (int8_t)guasa_read_mem(e), dy = (int8_t)guasa_read_mem(e + 1);
			int rx = guasa_read_mem(e + 2), ry = guasa_read_mem(e + 3);
			if (type == "attack" && guasa_read_mem(e + 5) >= 0x80) dx = -guasa_read_mem(e + 5);
			int cx = sx + (flip ? -dx : dx), cy = sy - dy;
			out.push_back({ obj, type, id, cx - rx, cy - ry, cx + rx, cy + ry });
		}
		if (proj) return;
		// throwable: +6C half width, +6E height, standing on the feet
		int trx = MemW(base + 0x6C), try_ = MemW(base + 0x6E);
		uint8_t st = guasa_read_mem(base + 0x03);
		bool hurt = guasa_read_mem(anim + 8) || guasa_read_mem(anim + 9) || guasa_read_mem(anim + 0xA);
		if (hurt && st != 0x0E && st != 0x14 && !guasa_read_mem(base + 0x143) && !guasa_read_mem(base + 0x1BF) && !guasa_read_mem(base + 0x1A1))
			out.push_back({ obj, guasa_read_mem(base + 0x181) ? "air_throwable" : "throwable", 0, sx - trx, sy - try_, sx + trx, sy });
		// throw: +64 s16 dx, +66 s16 dy, +68 rx, +6A ry (written by a throw attempt; stays there)
		int tdx = (int16_t)MemW(base + 0x64), tdy = (int16_t)MemW(base + 0x66), tx = MemW(base + 0x68), ty = MemW(base + 0x6A);
		if (tdx || tdy || tx || ty) {
			int cx = sx + (flip ? -tdx : tdx), cy = sy - tdy;
			out.push_back({ obj, "throw", 0, cx - tx, cy - ty, cx + tx, cy + ty });
		}
	};
	for (int p = 0; p < 2; p++) {
		uint32_t base = 0xFF844E + 0x400 * p;
		if (guasa_read_mem(base) > 0) object(p, base, false);
	}
	for (int i = 0; i < 24; i++) {
		uint32_t base = 0xFF97A2 + 0xC0 * i;
		uint16_t w = MemW(base);
		if (i < 8 ? w == 0x0101 : (w & 0xFF00) == 0x0100) object(10 + i, base, i < 8);
	}
	return out;
}

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "usage: core_test <roms> check|select|hashes|rollback|state [arg]\n"); return 2; }
	std::string mode = argv[2];
	if (mode == "romcheck") {
		// guasa_check_rom on the folder: its report, and exit code 0 only if nothing is missing
		char report[4000] = "";
		int n = guasa_check_rom(argv[1], report, sizeof(report));
		printf("%s", report);
		return n == 0 ? 0 : 1;
	}
	char msg[200] = "";
	if (guasa_init(argv[1], msg, sizeof(msg))) { printf("init failed: %s\n", msg); return 1; }

	if (mode == "check") {
		bool ok = CheckXxh64();
		printf("XXH64 test vectors: %s\n", ok ? "ok" : "WRONG");
		printf("fps %.2f, state %zu bytes\n", guasa_fps(), guasa_state_size());
		auto t0 = std::chrono::steady_clock::now();
		int n = guasa_boot_to_select();
		double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		printf("boot to select: %d frames in %.2f s\n", n, s);
		printf("starting state: state hash %016llx, game hash %016llx\n",
		       (unsigned long long)guasa_state_hash(), (unsigned long long)guasa_game_hash());
		printf("RAM: speed (FF83A9) %02x, difficulty (FF82D3) %02x\n", guasa_read_ram(0xFF83A9), guasa_read_ram(0xFF82D3));
		std::vector<uint8_t> buf(guasa_state_size());
		uint16_t in[2];
		t0 = std::chrono::steady_clock::now();
		for (int f = 0; f < 3600; f++) { Inputs(in); guasa_run_frame(in, 0); guasa_save(buf.data(), buf.size()); guasa_xxh64(buf.data(), buf.size()); }
		s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		printf("speed with a save and a hash every frame: %.1fx real time\n", 3600 / s / guasa_fps());
		guasa_exit();
		return ok ? 0 : 1;
	}

	if (mode == "select") {
		guasa_boot_to_select();
		// one more frame, drawn, with nothing pressed: the picture of the starting state
		uint16_t none[2] = { 0, 0 };
		guasa_run_frame(none, 1);
		int r = SaveBmp(argc > 3 ? argv[3] : "select.bmp");
		guasa_exit();
		return r;
	}

	if (mode == "explore") {
		// For practice mode: picks the first characters, waits for the fight, then
		// P1 walks right and left. Prints the words of P1's block (FF844E) that
		// follow the walk, and life, clock and the start flag along the way.
		guasa_boot_to_select();
		uint16_t in[2] = { 0, 0 };
		auto run = [&](int frames, uint16_t p1, uint16_t p2) { for (int f = 0; f < frames; f++) { in[0] = p1; in[1] = p2; guasa_run_frame(in, 0); } };
		auto word = [](uint32_t a) { return (uint16_t)(guasa_read_ram(a) << 8 | guasa_read_ram(a + 1)); };
		run(30, 0, 0); run(4, GUASA_LP, GUASA_LP); run(30, 0, 0); run(4, GUASA_LP, GUASA_LP);
		int f = 0;
		for (; f < 3000 && guasa_read_ram(0xFF8DCF) != 0x99; f++) run(1, 0, 0);
		run(240, 0, 0);
		printf("fight after %d frames; clock %02x; life P1 %02x%02x P2 %02x%02x; chars %02x %02x\n", f, guasa_read_ram(0xFF8DCE),
		       guasa_read_ram(0xFF844E + 0x2A), guasa_read_ram(0xFF844E + 0x2B), guasa_read_ram(0xFF884E + 0x2A), guasa_read_ram(0xFF884E + 0x2B),
		       guasa_read_ram(0xFF844E + 0x391), guasa_read_ram(0xFF884E + 0x391));
		std::vector<uint16_t> a(0x200), b(0x200), c(0x200);
		for (int i = 0; i < 0x200; i++) a[i] = word(0xFF844E + 2 * i);
		run(60, GUASA_RIGHT, 0);
		for (int i = 0; i < 0x200; i++) b[i] = word(0xFF844E + 2 * i);
		run(60, GUASA_LEFT, 0);
		for (int i = 0; i < 0x200; i++) c[i] = word(0xFF844E + 2 * i);
		for (int i = 0; i < 0x200; i++)
			if (b[i] > a[i] && c[i] < b[i] && b[i] - a[i] < 400)
				printf("P1 +%03x: %04x -> %04x -> %04x   P2 same place %04x\n", 2 * i, a[i], b[i], c[i], word(0xFF884E + 2 * i));
		// then P1 walks up to P2 and attacks: bytes that only grow (super meter, the
		// rival's dizziness) while P2's life goes down
		std::vector<uint8_t> s1(0x800), s2(0x800);
		auto snap = [&](std::vector<uint8_t>& s) { for (int i = 0; i < 0x800; i++) s[i] = guasa_read_ram(0xFF844E + i); };
		snap(s1);
		std::vector<int> grew(0x800, 0), shrank(0x800, 0);
		run(40, GUASA_RIGHT, 0);
		for (int k = 0; k < 40; k++) {
			std::vector<uint8_t> before(0x800);
			snap(before);
			run(3, GUASA_RIGHT | GUASA_HP, 0); run(12, GUASA_RIGHT, 0); run(3, GUASA_DOWN | GUASA_RIGHT, 0); run(3, GUASA_RIGHT | GUASA_HP, 0); run(12, 0, 0);
			snap(s2);
			for (int i = 0; i < 0x800; i++) { grew[i] += s2[i] > before[i]; shrank[i] += s2[i] < before[i]; }
		}
		printf("life after attacks: P1 %02x P2 %02x\n", guasa_read_ram(0xFF844E + 0x2B), guasa_read_ram(0xFF884E + 0x2B));
		for (int i = 0; i < 0x800; i++)
			if (grew[i] >= 3 && shrank[i] == 0 && s2[i] > s1[i])
				printf("%s +%03x: %02x -> %02x (grew %d times)\n", i < 0x400 ? "P1" : "P2", i & 0x3FF, s1[i], s2[i], grew[i]);
		run(1, 0, 0);
		in[0] = in[1] = 0;
		guasa_run_frame(in, 1);
		SaveBmp(argc > 3 ? argv[3] : "explore.bmp");
		guasa_exit();
		return 0;
	}

	if (mode == "frames") {
		// Frame meter research: picks Ryu (P1) and Ken (P2), waits until the clock
		// first ticks (99 -> 98), then runs the scenarios in argv[3]. Each scenario
		// starts from that same moment. Script lines:
		//   = name            a new scenario (back to the start of the fight)
		//   N p1 p2           N frames with these inputs (hex GUASA_* bits)
		//   bmp <path>        one frame with no input, drawn and saved
		//   mem <addr> <len>  prints len bytes there (hex; ROM or RAM, guasa_read_mem)
		// argv[4] gets, for every frame, a 16-byte header (scenario, frame, p1, p2
		// as little-endian u32), all 64 KB of work RAM after the frame, and 32
		// bytes of ROM at each player's animation pointer (P1 then P2).
		// Optional p1=<moves> p2=<moves>: cursor moves (L R U D) before picking,
		// to try other characters.
		if (argc < 5) { fprintf(stderr, "usage: core_test <roms> frames <script.txt> <out.bin> [p1=LRUD..] [p2=LRUD..]\n"); return 2; }
		std::string moves[2];
		for (int i = 5; i < argc; i++)
			if (argv[i][0] == 'p' && (argv[i][1] == '1' || argv[i][1] == '2') && argv[i][2] == '=') moves[argv[i][1] - '1'] = argv[i] + 3;
		guasa_boot_to_select();
		uint16_t in[2] = { 0, 0 };
		int f = PickAndFight(moves, 0);
		printf("clock %02x after %d frames; chars %02x %02x\n", guasa_read_ram(0xFF8DCE), f,
		       guasa_read_ram(0xFF844E + 0x391), guasa_read_ram(0xFF884E + 0x391));
		const size_t size = guasa_state_size();
		std::vector<uint8_t> start(size);
		guasa_save(start.data(), size);
		FILE* sc = fopen(argv[3], "r");
		FILE* out = fopen(argv[4], "wb");
		if (!sc || !out) { fprintf(stderr, "can't open the script or the output\n"); return 1; }
		std::vector<uint8_t> ram(0x10000);
		uint32_t scen = 0, frame = 0;
		auto record = [&]() {
			uint32_t hdr[4] = { scen, frame, in[0], in[1] };
			fwrite(hdr, 4, 4, out);
			for (uint32_t i = 0; i < 0x10000; i++) ram[i] = guasa_read_ram(0xFF0000 + i);
			fwrite(ram.data(), 1, ram.size(), out);
			// and 32 bytes of ROM at each player's animation pointer (long at +0x1A)
			for (uint32_t pl = 0; pl < 2; pl++) {
				uint32_t blk = 0xFF844E + 0x400 * pl, ptr = 0;
				for (int k = 0; k < 4; k++) ptr = ptr << 8 | guasa_read_ram(blk + 0x1A + k);
				uint8_t rom[32];
				for (int k = 0; k < 32; k++) rom[k] = guasa_read_mem(ptr + k);
				fwrite(rom, 1, 32, out);
			}
			frame++;
		};
		char line[512];
		while (fgets(line, sizeof(line), sc)) {
			unsigned n, a, b;
			char path[400];
			if (line[0] == '=') {
				guasa_load(start.data(), size);
				scen++; frame = 0;
				printf("scenario %u:%s", scen, line + 1);
			} else if (sscanf(line, "mem %x %x", &a, &b) == 2) {
				// prints b bytes at address a (ROM or RAM)
				for (unsigned k = 0; k < b; k++) {
					if (k % 16 == 0) printf("%s%06x:", k ? "\n" : "", a + k);
					printf(" %02x", guasa_read_mem(a + k));
				}
				printf("\n");
			} else if (sscanf(line, "bmp %399s", path) == 1) {
				in[0] = in[1] = 0;
				guasa_run_frame(in, 1);
				record();
				SaveBmp(path);
			} else if (sscanf(line, "%u %x %x", &n, &a, &b) == 3) {
				for (unsigned k = 0; k < n; k++) { in[0] = (uint16_t)a; in[1] = (uint16_t)b; guasa_run_frame(in, 0); record(); }
			}
		}
		fclose(sc);
		fclose(out);
		guasa_exit();
		return 0;
	}

	if (mode == "boxes") {
		// Hitbox viewer research: like "frames" (same start, same script lines,
		// p1=/p2= cursor moves) but every frame is drawn, and "bmp <path>" also
		// prints the boxes of ComputeBoxes() for that frame and the 3 before it:
		//   box <delay> <obj> <type> <id> <left> <top> <right> <bottom>
		// (obj 0/1 = P1/P2, 10+ = object slots; delay 0 = RAM after the drawn frame)
		// and "obj" lines with the raw values; "bmp <path> p1 p2" holds inputs on it.
		// "trace N p1 p2" runs N frames printing each player's state, anim pointer
		// and throw words (+64..+6F); "clearthrow 1" zeroes +64..+6B after every
		// frame; "hash" prints a hash of work RAM without those words.
		if (argc < 4) { fprintf(stderr, "usage: core_test <roms> boxes <script.txt> [p1=LRUD..] [p2=LRUD..]\n"); return 2; }
		std::string moves[2];
		for (int i = 4; i < argc; i++)
			if (argv[i][0] == 'p' && (argv[i][1] == '1' || argv[i][1] == '2') && argv[i][2] == '=') moves[argv[i][1] - '1'] = argv[i] + 3;
		guasa_boot_to_select();
		int f = PickAndFight(moves, 1);
		printf("clock %02x after %d frames; chars %02x %02x\n", guasa_read_ram(0xFF8DCE), f,
		       guasa_read_ram(0xFF844E + 0x391), guasa_read_ram(0xFF884E + 0x391));
		const size_t size = guasa_state_size();
		std::vector<uint8_t> start(size);
		guasa_save(start.data(), size);
		FILE* sc = fopen(argv[3], "r");
		if (!sc) { fprintf(stderr, "can't open the script\n"); return 1; }
		std::vector<std::vector<HitBox>> hist;   // hist.back() = after the last frame
		bool clearThrow = false;   // "clearthrow 1": zero +64..+6B after every frame, so a throw box shows only when written
		auto step = [&](uint16_t p1, uint16_t p2) {
			uint16_t in[2] = { p1, p2 };
			guasa_run_frame(in, 1);
			hist.push_back(ComputeBoxes());
			if (hist.size() > 4) hist.erase(hist.begin());
			if (clearThrow)
				for (int p = 0; p < 2; p++)
					for (int k = 0x64; k < 0x6C; k++) guasa_write_ram(0xFF844E + 0x400 * p + k, 0);
		};
		auto rawObj = [](int obj, uint32_t base) {
			uint32_t anim = MemL(base + 0x1A);
			printf("obj %d base %06x x %d y %d flip %d st %02x anim %06x tab %06x ids", obj, base, (int16_t)MemW(base + 6), (int16_t)MemW(base + 0xA),
			       guasa_read_mem(base + 0x12), guasa_read_mem(base + 3), anim, MemL(base + 0x34));
			for (int k = 8; k < 0x0E; k++) printf(" %02x", guasa_read_mem(anim + k));
			printf(" throw");
			for (int k = 0x64; k < 0x70; k += 2) printf(" %04x", MemW(base + k));
			if (int id = guasa_read_mem(anim + 0xC)) {   // the attack box entry, all 16 bytes
				uint32_t tab = MemL(base + 0x34), e = tab + (int16_t)MemW(tab + 6) + id * 0x10;
				printf(" atk %06x:", e);
				for (int k = 0; k < 16; k++) printf(" %02x", guasa_read_mem(e + k));
			}
			printf("\n");
		};
		char line[512];
		while (fgets(line, sizeof(line), sc)) {
			unsigned n, a, b;
			char path[400];
			if (line[0] == '=') {
				guasa_load(start.data(), size);
				hist.clear();
				printf("scenario:%s", line + 1);
			} else if (sscanf(line, "clearthrow %u", &n) == 1) {
				clearThrow = n != 0;
			} else if (!strncmp(line, "hash", 4)) {
				// work RAM without the throw words (+64..+6B of each player): does clearing them change the game?
				std::vector<uint8_t> ram(0x10000);
				for (uint32_t i = 0; i < 0x10000; i++) ram[i] = guasa_read_ram(0xFF0000 + i);
				for (int p = 0; p < 2; p++) memset(&ram[0x844E + 0x400 * p + 0x64], 0, 8);
				printf("hash %016llx\n", (unsigned long long)guasa_xxh64(ram.data(), ram.size()));
			} else if (sscanf(line, "mem %x %x", &a, &b) == 2) {
				for (unsigned k = 0; k < b; k++) {
					if (k % 16 == 0) printf("%s%06x:", k ? "\n" : "", a + k);
					printf(" %02x", guasa_read_mem(a + k));
				}
				printf("\n");
			} else if (sscanf(line, "bmp %399s", path) == 1) {
				unsigned q1 = 0, q2 = 0;   // optional inputs for that frame: bmp <path> p1 p2
				sscanf(line, "bmp %*s %x %x", &q1, &q2);
				step((uint16_t)q1, (uint16_t)q2);
				SaveBmp(path);
				printf("bmp %s camera %d %d\n", path, (int16_t)MemW(0xFF8ED4), (int16_t)MemW(0xFF8ED8));
				for (int p = 0; p < 2; p++) rawObj(p, 0xFF844E + 0x400 * p);
				for (int i = 0; i < 24; i++) {
					uint32_t base = 0xFF97A2 + 0xC0 * i;
					if (guasa_read_mem(base) == 1) rawObj(10 + i, base);
				}
				for (size_t d = 0; d < hist.size(); d++)
					for (auto& x : hist[hist.size() - 1 - d])
						printf("box %zu %d %s %d %d %d %d %d\n", d, x.obj, x.type.c_str(), x.id, x.l, x.t, x.r, x.b);
			} else if (sscanf(line, "trace %u %x %x", &n, &a, &b) == 3) {
				for (unsigned k = 0; k < n; k++) {
					step((uint16_t)a, (uint16_t)b);
					printf("t%u", k);
					for (int p = 0; p < 2; p++) {
						uint32_t base = 0xFF844E + 0x400 * p;
						printf(" | P%d st %02x.%02x anim %06x thr", p + 1, guasa_read_mem(base + 3), guasa_read_mem(base + 4), MemL(base + 0x1A));
						for (int k2 = 0x64; k2 < 0x70; k2 += 2) printf(" %04x", MemW(base + k2));
					}
					printf("\n");
				}
			} else if (sscanf(line, "%u %x %x", &n, &a, &b) == 3) {
				for (unsigned k = 0; k < n; k++) step((uint16_t)a, (uint16_t)b);
			}
		}
		fclose(sc);
		guasa_exit();
		return 0;
	}

	if (mode == "state") {
		guasa_boot_to_select();
		std::vector<uint8_t> buf(guasa_state_size());
		size_t n = guasa_save(buf.data(), buf.size());
		FILE* fp = fopen(argc > 3 ? argv[3] : "state.raw", "wb");
		if (!fp) return 1;
		fwrite(buf.data(), 1, n, fp);
		fclose(fp);
		printf("%016llx\n", (unsigned long long)guasa_state_hash());
		guasa_exit();
		return 0;
	}

	if (mode == "coins") {
		// Puts in N coins for P1 after the boot (no start) and prints work RAM
		// (all 64 KB), to find where the game keeps the credits.
		int n = argc > 3 ? atoi(argv[3]) : 1;
		for (int f = 0; f < 1600; f++) {
			uint16_t in[2] = { 0, 0 };
			for (int c = 0; c < n; c++) if (f >= 1000 + c * 20 && f < 1006 + c * 20) in[0] = GUASA_COIN;
			guasa_run_frame(in, f == 1599 && argc > 4);   // argv[4]: also save the last frame as BMP
		}
		if (argc > 4) SaveBmp(argv[4]);
		for (uint32_t a = 0xFF0000; a < 0x1000000; a++) printf("%02x", guasa_read_ram(a));
		printf("\n");
		guasa_exit();
		return 0;
	}

	if (mode == "cert") {
		// Certification kit (core/cert/): runs the inputs in argv[3] (one frame per
		// line, "p1 p2" in hex) and writes the 64 KB of 68000 RAM after every frame
		// to argv[4]. With state=<raw> it first loads a decompressed Fightcade .fs;
		// otherwise it starts from the cold boot.
		if (argc < 5) { fprintf(stderr, "usage: core_test <roms> cert <inputs.txt> <out.bin> [state=<raw>] [bmp=<path>]\n"); return 2; }
		const char* statePath = NULL;
		const char* bmpPath = NULL;   // bmp=<path>: also saves the last frame, to see where the inputs got to
		for (int i = 5; i < argc; i++) {
			if (!strncmp(argv[i], "state=", 6)) statePath = argv[i] + 6;
			if (!strncmp(argv[i], "bmp=", 4)) bmpPath = argv[i] + 4;
		}
		if (statePath) {
			FILE* sf = fopen(statePath, "rb");
			if (!sf) { fprintf(stderr, "can't read %s\n", statePath); return 1; }
			std::vector<uint8_t> raw;
			uint8_t chunk[65536];
			size_t got;
			while ((got = fread(chunk, 1, sizeof(chunk), sf)) > 0) raw.insert(raw.end(), chunk, chunk + got);
			fclose(sf);
			if (guasa_load_state_file(raw.data(), raw.size())) { fprintf(stderr, "the state file doesn't fit the core\n"); return 1; }
		}
		FILE* in = fopen(argv[3], "r");
		FILE* out = fopen(argv[4], "wb");
		if (!in || !out) { fprintf(stderr, "can't open the inputs or the output\n"); return 1; }
		unsigned a, b;
		int n = 0;
		std::vector<uint8_t> ram(0x10000);
		while (fscanf(in, "%x %x", &a, &b) == 2) {
			uint16_t x[2] = { (uint16_t)a, (uint16_t)b };
			guasa_run_frame(x, bmpPath != NULL);
			for (uint32_t i = 0; i < 0x10000; i++) ram[i] = guasa_read_ram(0xFF0000 + i);
			fwrite(ram.data(), 1, ram.size(), out);
			n++;
		}
		fclose(in);
		fclose(out);
		if (bmpPath) SaveBmp(bmpPath);
		printf("%d frames, game hash at the end %016llx\n", n, (unsigned long long)guasa_game_hash());
		guasa_exit();
		return 0;
	}

	int frames = argc > 3 ? atoi(argv[3]) : 3600;

	if (mode == "hashes") {
		guasa_boot_to_select();
		uint16_t in[2];
		for (int f = 0; f < frames; f++) {
			Inputs(in);
			guasa_run_frame(in, 0);
			printf("%d %016llx %016llx\n", f, (unsigned long long)guasa_state_hash(), (unsigned long long)guasa_game_hash());
		}
		guasa_exit();
		return 0;
	}

	if (mode == "rollback") {
		guasa_boot_to_select();
		const size_t size = guasa_state_size();
		// reference run
		std::vector<uint8_t> start(size);
		guasa_save(start.data(), size);
		uint64_t rngStart = g_rng;
		std::vector<uint64_t> ref;
		uint16_t in[2];
		for (int f = 0; f < frames; f++) { Inputs(in); guasa_run_frame(in, 0); ref.push_back(guasa_state_hash()); }

		// the same inputs, going back 7 frames and resimulating on every frame
		guasa_load(start.data(), size);
		g_rng = rngStart;
		std::vector<std::vector<uint8_t>> ring;
		std::vector<std::vector<uint16_t>> inputs;
		int bad = -1;
		for (int f = 0; f < frames && bad < 0; f++) {
			std::vector<uint8_t> s(size);
			guasa_save(s.data(), size);
			ring.push_back(s);
			Inputs(in);
			inputs.push_back({ in[0], in[1] });
			if (ring.size() > 8) { ring.erase(ring.begin()); inputs.erase(inputs.begin()); }
			guasa_load(ring[0].data(), size);
			for (auto& i : inputs) { uint16_t x[2] = { i[0], i[1] }; guasa_run_frame(x, 0); }
			if (guasa_state_hash() != ref[f]) bad = f;
		}
		printf("rollback of 7 frames on every frame, %d frames: %s", frames, bad < 0 ? "same hashes\n" : "DIFFERENT");
		if (bad >= 0) printf(" at frame %d\n", bad);
		guasa_exit();
		return bad < 0 ? 0 : 1;
	}

	fprintf(stderr, "unknown mode %s\n", mode.c_str());
	return 2;
}
