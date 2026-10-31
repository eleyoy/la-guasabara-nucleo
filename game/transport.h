// The game's UDP: one socket that talks to the other player directly or
// through the hub's relay, whichever is faster (PLAN.md section 5).
//
// With a relay (an online set from the hub):
//   1. both games register with the relay using their set's code; the relay
//      answers only valid codes and tells each one the other's public address;
//   2. for 1.5 s both send probes directly (which also opens the routers: hole
//      punching) and through the relay, and measure both round trips;
//   3. player 1 picks the route (direct if it works and is no slower, else the
//      relay; always the relay if the admin set it for one of them) and tells
//      player 2. Each side receives on both routes, and a small packet every
//      second keeps the direct route open.
// Without a relay (tests: guasa-game online ... <ip:port>): direct only.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct TransportConfig {
	int local = 0;               // 0 = player 1 (picks the route), 1 = player 2
	int port = 7100;             // local UDP port
	std::string peer;            // "ip:port" for direct only
	std::string relay;           // "host:port" of the hub's relay
	std::vector<uint8_t> token;  // this player's code for the set (16 bytes)
	bool relayOnly = false;      // the admin's "only relay" for one of the two
	int lagMs = 0, jitterMs = 0, lossPct = 0;   // bad-internet simulator, for tests
};

class Transport {
public:
	enum Route { None, Direct, Relay };

	bool Open(const TransportConfig& cfg, std::string* error);
	// Steps 1-3 above (only with a relay). Returns false if not even the relay answers.
	bool Negotiate(std::string* error);
	void Close();

	// Game packets (GekkoNet's), to and from the other player.
	void Send(const uint8_t* data, int len);
	// Everything that arrived from the other player since the last call.
	std::vector<std::vector<uint8_t>> Receive();

	Route route() const { return m_route; }
	int directMs() const { return m_directMs; }   // -1: the direct route didn't answer
	int relayMs() const { return m_relayMs; }
	std::string Describe() const;                  // "directa, 23 ms (por el servidor 41 ms)"

private:
	struct Addr { uint32_t ip = 0; uint16_t port = 0; bool set = false; };   // network byte order
	void SendRaw(const uint8_t* data, int len, const Addr& to, bool simulate);
	void SendInner(uint8_t type, const uint8_t* data, int len, Route via);
	void Pump(std::vector<std::vector<uint8_t>>* games);
	void HandleInner(const uint8_t* d, int len, std::vector<std::vector<uint8_t>>* games, Route from);
	void FlushLagged();

	TransportConfig m_cfg;
	intptr_t m_sock = -1;
	Addr m_peer, m_relay;
	Route m_route = None;
	int m_directMs = -1, m_relayMs = -1;
	bool m_routeKnown = false;
	uint64_t m_lastKeepalive = 0;
	struct Lagged { uint64_t at; Addr to; std::vector<uint8_t> data; };
	std::vector<Lagged> m_lagged;
	uint64_t m_rng = 99;
	std::vector<int> m_directRtts, m_relayRtts;
	std::vector<std::vector<uint8_t>> m_pendingGames;   // game packets that came while negotiating
};

uint64_t NowMs();
