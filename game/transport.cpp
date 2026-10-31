#include "transport.h"
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <timeapi.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)   // Windows' value; MinGW doesn't define it
#endif
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
typedef int SOCKET;
#endif

// Packets:
//   to the relay:     "GR" 'R' token            register (the relay answers 'O' with our
//                                               address, and 'P' with the other's)
//                     "GR" 'F' token inner      pass inner to the other player
//   from the relay:   "GR" 'O' "ip:port" | "GR" 'P' "ip:port" | "GR" 'f' inner
//   direct:           "GP" inner
//   inner:            'g' game data | 'p' t / 'r' t (probe and answer, t = 8 bytes)
//                     'd' route / 'a' (player 1's choice and its ack) | 'k' (keep the route open)

uint64_t NowMs()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static bool ParseAddr(const std::string& s, uint32_t* ip, uint16_t* port)
{
	size_t colon = s.rfind(':');
	if (colon == std::string::npos) return false;
	std::string host = s.substr(0, colon);
	int p = atoi(s.c_str() + colon + 1);
	if (p <= 0 || p > 65535) return false;
	addrinfo hints{}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
	*ip = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
	*port = htons((uint16_t)p);
	freeaddrinfo(res);
	return true;
}

static std::string AddrText(uint32_t ip, uint16_t port)
{
	char buf[64];
	in_addr a;
	a.s_addr = ip;
	inet_ntop(AF_INET, &a, buf, sizeof(buf));
	return std::string(buf) + ":" + std::to_string(ntohs(port));
}

static int Median(std::vector<int> v)
{
	if (v.empty()) return -1;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

bool Transport::Open(const TransportConfig& cfg, std::string* error)
{
	m_cfg = cfg;
#ifdef _WIN32
	static bool started = false;
	if (!started) {
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
		timeBeginPeriod(1);   // 1 ms sleeps: Windows' default ~15 ms would skew the route measurement
		started = true;
	}
#endif
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((uint16_t)cfg.port);
	if (s == (SOCKET)-1 || bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
		if (error) *error = "no se pudo abrir el puerto UDP " + std::to_string(cfg.port);
		return false;
	}
#ifdef _WIN32
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
	BOOL off = FALSE;   // Windows reports "port unreachable" as an error on the next receive; ignore that
	DWORD ret = 0;
	WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
	m_sock = (intptr_t)s;
	if (!cfg.relay.empty()) {
		if (!ParseAddr(cfg.relay, &m_relay.ip, &m_relay.port)) {
			if (error) *error = "no se encontró el servidor " + cfg.relay;
			return false;
		}
		m_relay.set = true;
	} else {
		if (!ParseAddr(cfg.peer, &m_peer.ip, &m_peer.port)) {
			if (error) *error = "dirección no válida: " + cfg.peer;
			return false;
		}
		m_peer.set = true;
		m_route = Direct;
		m_routeKnown = true;
	}
	return true;
}

void Transport::Close()
{
	if (m_sock < 0) return;
#ifdef _WIN32
	closesocket((SOCKET)m_sock);
#else
	close((int)m_sock);
#endif
	m_sock = -1;
}

void Transport::SendRaw(const uint8_t* data, int len, const Addr& to, bool simulate)
{
	if (!to.set) return;
	if (simulate && (m_cfg.lagMs || m_cfg.jitterMs || m_cfg.lossPct)) {
		m_rng = m_rng * 6364136223846793005ULL + 1442695040888963407ULL;
		uint32_t r = (uint32_t)(m_rng >> 33);
		if (m_cfg.lossPct && (int)(r % 100) < m_cfg.lossPct) return;
		int ms = m_cfg.lagMs + (m_cfg.jitterMs ? (int)((r >> 8) % (m_cfg.jitterMs + 1)) : 0);
		m_lagged.push_back({ NowMs() + ms, to, std::vector<uint8_t>(data, data + len) });
		return;
	}
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = to.ip;
	a.sin_port = to.port;
	sendto((SOCKET)m_sock, (const char*)data, len, 0, (sockaddr*)&a, sizeof(a));
}

void Transport::FlushLagged()
{
	uint64_t now = NowMs();
	for (size_t i = 0; i < m_lagged.size();) {
		if (m_lagged[i].at <= now) {
			Lagged l = m_lagged[i];
			m_lagged.erase(m_lagged.begin() + i);
			SendRaw(l.data.data(), (int)l.data.size(), l.to, false);
		} else i++;
	}
}

// One inner packet to the other player, directly or through the relay.
void Transport::SendInner(uint8_t type, const uint8_t* data, int len, Route via)
{
	std::vector<uint8_t> p;
	if (via == Direct) {
		p = { 'G', 'P', type };
		p.insert(p.end(), data, data + len);
		SendRaw(p.data(), (int)p.size(), m_peer, true);
	} else {
		p = { 'G', 'R', 'F' };
		p.insert(p.end(), m_cfg.token.begin(), m_cfg.token.end());
		p.push_back(type);
		p.insert(p.end(), data, data + len);
		SendRaw(p.data(), (int)p.size(), m_relay, true);
	}
}

void Transport::HandleInner(const uint8_t* d, int len, std::vector<std::vector<uint8_t>>* games, Route from)
{
	if (len < 1) return;
	uint8_t t = d[0];
	if (t == 'g') {
		if (games) games->emplace_back(d + 1, d + len);
	} else if (t == 'p' && len == 9) {
		SendInner('r', d + 1, 8, from);   // answer on the same route
	} else if (t == 'r' && len == 9) {
		uint64_t sent;
		memcpy(&sent, d + 1, 8);
		int rtt = (int)(NowMs() - sent);
		(from == Direct ? m_directRtts : m_relayRtts).push_back(rtt);
	} else if (t == 'd' && len == 2 && m_cfg.local == 1) {
		m_route = d[1] == Direct ? Direct : Relay;
		m_routeKnown = true;
		SendInner('a', nullptr, 0, Relay);
	} else if (t == 'a') {
		m_routeKnown = true;   // player 2 has the choice
	}
}

void Transport::Pump(std::vector<std::vector<uint8_t>>* games)
{
	FlushLagged();
	uint8_t buf[2048];
	for (;;) {
		sockaddr_in from{};
		socklen_t fl = sizeof(from);
		int n = (int)recvfrom((SOCKET)m_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
		if (n < 0) {
#ifdef _WIN32
			if (WSAGetLastError() == WSAEWOULDBLOCK) break;
			continue;
#else
			break;
#endif
		}
		if (n < 3 || buf[0] != 'G') continue;
		bool fromRelay = m_relay.set && from.sin_addr.s_addr == m_relay.ip && from.sin_port == m_relay.port;
		bool fromPeer = m_peer.set && from.sin_addr.s_addr == m_peer.ip && from.sin_port == m_peer.port;
		if (buf[1] == 'P' && fromPeer) {
			HandleInner(buf + 2, n - 2, games, Direct);
		} else if (buf[1] == 'R' && fromRelay) {
			if (buf[2] == 'f') HandleInner(buf + 3, n - 3, games, Relay);
			else if (buf[2] == 'P' && !m_peer.set) {
				std::string text((char*)buf + 3, n - 3);
				m_peer.set = ParseAddr(text, &m_peer.ip, &m_peer.port);
			}
		}
	}
	// keep the direct route open (both routers) once it has answered
	uint64_t now = NowMs();
	if (m_relay.set && m_directMs >= 0 && now - m_lastKeepalive >= 1000) {
		SendInner('k', nullptr, 0, Direct);
		m_lastKeepalive = now;
	}
}

bool Transport::Negotiate(std::string* error)
{
	if (!m_relay.set) return true;
	std::vector<std::vector<uint8_t>> early;   // game packets that come before we finish
	auto sleep = [] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); };   // short: it adds to the measured time

	// 1. register until the relay tells us the other player's address
	std::vector<uint8_t> reg = { 'G', 'R', 'R' };
	reg.insert(reg.end(), m_cfg.token.begin(), m_cfg.token.end());
	uint64_t start = NowMs(), lastReg = 0;
	while (!m_peer.set) {
		if (NowMs() - start > 20000) {
			if (error) *error = "el otro jugador no llegó (ni por el servidor)";
			return false;
		}
		if (NowMs() - lastReg >= 250) { SendRaw(reg.data(), (int)reg.size(), m_relay, false); lastReg = NowMs(); }
		Pump(&early);
		sleep();
	}

	// 2. probe both routes for 1.5 s (the direct probes also open the routers)
	uint64_t probeEnd = NowMs() + 1500, lastProbe = 0;
	while (NowMs() < probeEnd) {
		if (NowMs() - lastProbe >= 100) {
			uint64_t t = NowMs();
			SendInner('p', (uint8_t*)&t, 8, Direct);
			SendInner('p', (uint8_t*)&t, 8, Relay);
			lastProbe = t;
		}
		Pump(&early);
		sleep();
	}
	m_directMs = Median(m_directRtts);
	m_relayMs = Median(m_relayRtts);

	// 3. player 1 picks and tells player 2; player 2 waits for it (else: relay)
	if (m_cfg.local == 0) {
		bool direct = !m_cfg.relayOnly && m_directMs >= 0 && (m_relayMs < 0 || m_directMs <= m_relayMs + 5);
		m_route = direct ? Direct : Relay;
		m_routeKnown = false;
		uint8_t choice = (uint8_t)m_route;
		uint64_t until = NowMs() + 1500, last = 0;
		while (!m_routeKnown && NowMs() < until) {
			if (NowMs() - last >= 100) { SendInner('d', &choice, 1, Relay); last = NowMs(); }
			Pump(&early);
			sleep();
		}
	} else {
		uint64_t until = NowMs() + 3000;
		while (!m_routeKnown && NowMs() < until) {
			Pump(&early);
			sleep();
		}
		if (!m_routeKnown) m_route = Relay;
	}
	if (m_route == Relay && m_relayMs < 0 && m_directMs < 0) {
		if (error) *error = "no hay ruta hacia el otro jugador";
		return false;
	}
	for (auto& g : early) m_pendingGames.push_back(g);
	return true;
}

void Transport::Send(const uint8_t* data, int len)
{
	SendInner('g', data, len, m_route == Direct ? Direct : Relay);
}

std::vector<std::vector<uint8_t>> Transport::Receive()
{
	std::vector<std::vector<uint8_t>> games;
	games.swap(m_pendingGames);
	Pump(&games);
	return games;
}

std::string Transport::Describe() const
{
	if (!m_relay.set) return "directa";
	auto ms = [](int v) { return v < 0 ? std::string("sin respuesta") : std::to_string(v) + " ms"; };
	if (m_route == Direct) return "directa, " + ms(m_directMs) + " (por el servidor " + ms(m_relayMs) + ")";
	return "por el servidor, " + ms(m_relayMs) + " (directa " + ms(m_directMs) + ")";
}
