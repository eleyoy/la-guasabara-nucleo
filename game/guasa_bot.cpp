// guasa-bot: the other side of an online match, with no window or sound, for
// tests (the Ryzen plays it against the organizer's PC). Same netplay code as
// the game window (net.cpp).
//
//   guasa-bot <roms> <1|2> <local_port> (<ip:port> | relay=<host:port> token=<hex> [relayonly])
//             [frames=N] [delay=N|auto] [seed=N] [lag=MS] [jitter=MS] [loss=PCT]
//
// frames=0 (the default) plays until the other side leaves. Prints "resultado 1|2"
// when a game ends, like the window.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <thread>
#include <string>
#include "guasa_core.h"
#include "net.h"
#include "results.h"
#ifdef _WIN32
#include <windows.h>
#endif

// Mashes directions and buttons, and taps Start every 20 seconds to continue
// after a match (the starting state already has 16 credits).
static uint16_t BotInput(int frame, uint64_t& rng)
{
	uint16_t in = 0;
	for (int k = 0; k < 10; k++) {
		rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
		if ((uint32_t)(rng >> 33) % 100 < 30) in |= 1 << k;
	}
	if (frame % 1200 < 6) in |= GUASA_START;
	return in;
}

int main(int argc, char** argv)
{
	NetConfig cfg;
	int frames = 0, pauseAt = -1, resumeAt = -1;   // pausaen=N seguiren=N: ask for the pause / to go on, at those frames (tests)
	uint64_t rng = 0;
	if (argc >= 4) {
		cfg.local = atoi(argv[2]) - 1;
		cfg.port = atoi(argv[3]);
		rng = 1000 + cfg.local;
	}
	for (int i = 4; i < argc; i++) {
		if (!strncmp(argv[i], "frames=", 7)) frames = atoi(argv[i] + 7);
		else if (!strncmp(argv[i], "seed=", 5)) rng = strtoull(argv[i] + 5, NULL, 10);
		else if (!strncmp(argv[i], "pausaen=", 8)) pauseAt = atoi(argv[i] + 8);
		else if (!strncmp(argv[i], "seguiren=", 9)) resumeAt = atoi(argv[i] + 9);
		else if (!ParseNetArg(argv[i], cfg)) { fprintf(stderr, "opcion desconocida: %s\n", argv[i]); return 2; }
	}
	if (argc < 5 || cfg.local < 0 || cfg.local > 1 || (cfg.remote.empty() && (cfg.relay.empty() || cfg.token.size() != 16))) {
		fprintf(stderr, "uso: guasa-bot <roms> <1|2> <puerto_local> (<ip:puerto> | relay=<host:puerto> token=<32 hex> [relayonly]) [frames=N] [delay=N|auto] [seed=N] [lag=MS] [jitter=MS] [loss=PCT] [pausa] [pausaen=N] [seguiren=N]\n");
		return 2;
	}

	char msg[300] = "";
	if (guasa_init(argv[1], msg, sizeof(msg))) { printf("error: %s\n", msg); return 1; }
	guasa_boot_to_select();
	printf("estado inicial %016llx\n", (unsigned long long)guasa_state_hash());

	NetMatch net;
	std::string err;
	if (!net.Start(cfg, &err)) { printf("error: %s\n", err.c_str()); return 1; }
	printf("P%d en el puerto %d, ruta %s, delay %s%s\n", cfg.local + 1, cfg.port, net.RouteText().c_str(),
	       cfg.delay < 0 ? "automatico" : std::to_string(cfg.delay).c_str(),
	       cfg.lagMs || cfg.jitterMs || cfg.lossPct ? " (simulando mala conexion)" : "");
	fflush(stdout);
#ifdef _WIN32
	timeBeginPeriod(1);
#endif

	const auto frameTime = std::chrono::nanoseconds((long long)(1e9 / guasa_fps()));
	auto next = std::chrono::steady_clock::now(), tStart = next;
	int localFrame = 0, shownDelay = -1, pausePhase = 0;
	bool wasPlaying = false;
	ResultWatcher results;
	double pingSum = 0;
	int pingN = 0;
	unsigned pingMax = 0;
	for (;;) {
		bool shown = false;
		uint16_t in = BotInput(localFrame, rng);
		if (localFrame == pauseAt || localFrame == resumeAt) in |= kPauseBit;
		localFrame++;
		NetMatch::Status st = net.Step(in, false, &shown);
		const NetStats& s = net.Stats();
		if (net.Pause().phase != pausePhase) {
			pausePhase = net.Pause().phase;
			printf("pausa fase %d (la pidio P%d) en el cuadro %d\n", pausePhase, net.Pause().by + 1, s.frame);
			fflush(stdout);
		}
		if (st == NetMatch::Playing && !wasPlaying) {
			printf("empezo la partida\n");
			fflush(stdout);
			wasPlaying = true;
			tStart = std::chrono::steady_clock::now();
		}
		if (shown) {
			if (int w = results.Update()) { printf("resultado %d\n", w); fflush(stdout); }
		}
		if (s.delay != shownDelay) { printf("delay %d (ping %u ms)\n", s.delay, s.ping); fflush(stdout); shownDelay = s.delay; }
		if (shown && s.frame % 60 == 0 && s.ping) {
			pingSum += s.ping;
			pingN++;
			if (s.ping > pingMax) pingMax = s.ping;
		}
		if (st == NetMatch::Ended) { printf("%s en el cuadro %d\n", net.EndReason().c_str(), s.frame); break; }
		if (frames > 0 && s.frame >= frames) break;

		next += std::chrono::duration_cast<std::chrono::nanoseconds>(frameTime * net.PaceFactor());
		auto now = std::chrono::steady_clock::now();
		if (next > now) std::this_thread::sleep_for(next - now);
		else if (now - next > frameTime * 10) next = now;   // don't race to catch up after a stall
	}

	const NetStats& s = net.Stats();
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
	printf("fin: %d cuadros en %.1f s (%.2f por segundo); %d rollbacks, %d cuadros resimulados, el mas largo %d; "
	       "ping promedio %.1f ms, maximo %u ms; delay %d; desincronizaciones: %d\n",
	       s.frame, secs, s.frame / (secs > 0 ? secs : 1), s.rollbacks, s.resimulated, s.maxRollback,
	       pingN ? pingSum / pingN : 0.0, pingMax, s.delay, s.desyncs);
	if (s.desyncs) printf("primera desincronizacion en el cuadro %d\n", s.firstDesync);
	net.Stop();
	guasa_exit();
	return s.desyncs ? 1 : 0;
}
