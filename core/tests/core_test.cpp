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
		auto run = [&](int frames, uint16_t p1, uint16_t p2) { for (int f = 0; f < frames; f++) { in[0] = p1; in[1] = p2; guasa_run_frame(in, 0); } };
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
