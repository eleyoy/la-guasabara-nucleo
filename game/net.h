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
	bool pause = false;          // casual sets: either player can pause (kPauseBit, below)
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

// The pause of casual sets. Asking for it is one more bit of the buttons, so it
// reaches both games on the same frame; its state is part of the game's state
// (saved with it), so rollbacks redo it too and the two games never differ.
// Up to 5 minutes; then, or when either player asks, a countdown and the game goes on.
constexpr uint16_t kPauseBit = 0x8000;
struct PauseState {
	int32_t phase = 0;           // 0 playing, 1 paused, 2 counting down to go on
	int32_t by = -1;             // who paused: 0 player 1, 1 player 2
	int32_t frames = 0;          // paused: frames so far; counting down: frames left
};

// One frame of the pause's rules, the same for both games and for replays.
// in: both players' buttons (with kPauseBit). True = the game stays still this frame.
bool PauseRule(PauseState& p, bool allowed, const uint16_t in[2]);

// A replay: both players' buttons on every frame from the starting state
// (character select, the same on every PC), pause bit included. With the same
// core, playing them back gives exactly the same game.
// File: "GUASAREP", version (u16) 1, flags (u16, 1 = the pause was allowed),
// frames (u32), then per frame player 1's and player 2's buttons (u16 each);
// all little-endian.
struct Replay {
	bool pause = false;
	std::vector<uint32_t> frames;   // player 1 | player 2 << 16
};
bool SaveReplay(const std::string& path, const Replay& r);
// One frame of a replay: the pause's rules, then the game (unless paused).
// True if the game moved.
bool ReplayFrame(const Replay& rep, size_t i, PauseState& ps, bool draw);
bool LoadReplay(const std::string& path, Replay& r, std::string* error);
// Frames as text, to send them live to spectators: little-endian u32 each, in base64.
std::string FramesToBase64(const uint32_t* frames, size_t n);
bool FramesFromBase64(const std::string& text, std::vector<uint32_t>& out);

class NetMatch {
public:
	enum Status { Waiting, Playing, Ended };

	// Opens the UDP port, finds the route to the other player (transport.h) and
	// starts the session. Takes up to a few seconds with a relay. The game must
	// already be at its starting state (guasa_boot_to_select), the same on both PCs.
	bool Start(const NetConfig& cfg, std::string* error);
	std::string RouteText() const { return m_transport.Describe(); }
	bool Relayed() const { return m_transport.route() == Transport::Relay; }

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
	const PauseState& Pause() const { return m_pause; }
	// The set so far as a replay: the buttons the game used on each frame (a
	// rollback rewrites the frames it redoes, so they end up as they were played).
	Replay Recording() const;
	const std::vector<uint32_t>& Recorded() const { return m_record; }

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
	PauseState m_pause;
	std::vector<uint32_t> m_record;
	int m_recordBase = -1;           // GekkoNet's number for the first frame
};

// The online options of the window and the bot, one per argument:
//   ip:port (direct, tests) | relay=host:port token=<32 hex> [relayonly]
//   delay=N|auto  lag=MS  jitter=MS  loss=PCT
// Returns false if the argument isn't one of these.
bool ParseNetArg(const char* arg, NetConfig& cfg);

// Automatic delay: half the round trip, in frames, rounded, at least 1 (LAN 1,
// 100 ms ~3, 150 ms ~4). To compare with what Fightcade picks during the alpha.
int AutoDelay(unsigned pingMs, double fps);
