// Online match over GekkoNet (rollback), on top of guasa-core. No SDL here: the
// game window (guasa_game.cpp) and the headless bot (guasa_bot.cpp) share it.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "transport.h"

struct NetConfig {
	int local = 0;               // 0 = player 1, 1 = player 2
	int port = 7100;             // local UDP port
	std::string remote;          // "ip:port" of the other player (direct only, for tests)
	std::string relay;           // "host:port" of the hub's relay (online sets)
	std::vector<uint8_t> token;  // this player's code for the set
	bool relayOnly = false;
	int delay = -1;              // frames of input delay; -1 = automatic from the ping
	int lagMs = 0, jitterMs = 0, lossPct = 0;   // bad-internet simulator, for tests
};

struct NetStats {
	bool started = false;
	int frame = 0;               // last confirmed-to-show frame
	int delay = 0;
	unsigned ping = 0;           // ms, round trip
	float jitter = 0;            // ms
	float framesAhead = 0;
	int rollbacks = 0, lastRollback = 0, maxRollback = 0, resimulated = 0;
	int desyncs = 0;
	int firstDesync = -1;
};

class NetMatch {
public:
	enum Status { Waiting, Playing, Ended };

	// Opens the UDP port, finds the route to the other player (transport.h) and
	// starts the session. Takes up to a few seconds with a relay. The game must
	// already be at its starting state (guasa_boot_to_select), the same on both PCs.
	bool Start(const NetConfig& cfg, std::string* error);
	std::string RouteText() const { return m_transport.Describe(); }

	// One frame: sends this PC's buttons, runs what GekkoNet asks (save, load,
	// advance, including rollbacks). draw: draw the frames that will be shown.
	// shown is set when a new frame was shown (play its sound then).
	Status Step(uint16_t localInput, bool draw, bool* shown);

	// Tells the other side we're leaving and closes the port.
	void Stop();

	// How long the next frame should take, relative to normal: a little slower
	// while this PC is ahead of the other one, so both stay in step.
	double PaceFactor() const;

	void SetDelay(int delay);
	const NetStats& Stats() const { return m_stats; }
	const std::string& EndReason() const { return m_endReason; }
	// The buttons the game used on the last shown frame, both players.
	const uint16_t* LastInputs() const { return m_lastInputs; }

private:
	struct GekkoSession* m_session = nullptr;
	Transport m_transport;
	NetConfig m_cfg;
	NetStats m_stats;
	int m_remoteHandle = -1;
	int m_run = 0;
	bool m_autoDelaySet = false;
	int m_startedAtFrame = 0;
	std::string m_endReason;
	uint16_t m_lastInputs[2] = { 0, 0 };
};

// The online options of the window and the bot, one per argument:
//   ip:port (direct, tests) | relay=host:port token=<32 hex> [relayonly]
//   delay=N|auto  lag=MS  jitter=MS  loss=PCT
// Returns false if the argument isn't one of these.
bool ParseNetArg(const char* arg, NetConfig& cfg);

// Automatic delay: half the round trip, in frames, rounded, at least 1 (LAN 1,
// 100 ms ~3, 150 ms ~4). To compare with what Fightcade picks during the alpha.
int AutoDelay(unsigned pingMs, double fps);
