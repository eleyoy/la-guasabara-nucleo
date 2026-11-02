#include "net.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <chrono>
#include <thread>
#include <vector>
#include "gekkonet.h"
#include "guasa_core.h"

// ---------------------------------------------------------------------------
// GekkoNet talks to the other player through our transport (transport.h): to it
// the other player is always the address "peer".

static Transport* g_transport;
static const char kPeer[] = "peer";
static std::vector<GekkoNetResult*> g_results;

static void AdapterSend(GekkoNetAddress*, const char* data, int length)
{
	g_transport->Send((const uint8_t*)data, length);
}

static GekkoNetResult** AdapterReceive(int* length)
{
	g_results.clear();
	for (auto& p : g_transport->Receive()) {
		GekkoNetResult* r = (GekkoNetResult*)malloc(sizeof(GekkoNetResult));
		r->addr.size = sizeof(kPeer) - 1;
		r->addr.data = malloc(r->addr.size);
		memcpy(r->addr.data, kPeer, r->addr.size);
		r->data_len = (unsigned int)p.size();
		r->data = malloc(p.size() ? p.size() : 1);
		memcpy(r->data, p.data(), p.size());
		g_results.push_back(r);
	}
	*length = (int)g_results.size();
	return g_results.data();
}

static void AdapterFree(void* p) { free(p); }

static GekkoNetAdapter g_adapter = { AdapterSend, AdapterReceive, AdapterFree };

// ---------------------------------------------------------------------------

bool ParseNetArg(const char* a, NetConfig& cfg)
{
	if (!strncmp(a, "delay=", 6)) cfg.delay = strcmp(a + 6, "auto") ? atoi(a + 6) : -1;
	else if (!strncmp(a, "lag=", 4)) cfg.lagMs = atoi(a + 4);
	else if (!strncmp(a, "jitter=", 7)) cfg.jitterMs = atoi(a + 7);
	else if (!strncmp(a, "loss=", 5)) cfg.lossPct = atoi(a + 5);
	else if (!strncmp(a, "relay=", 6)) cfg.relay = a + 6;
	else if (!strcmp(a, "relayonly")) cfg.relayOnly = true;
	else if (!strcmp(a, "pausa")) cfg.pause = true;
	else if (!strncmp(a, "token=", 6)) {
		cfg.token.clear();
		for (const char* p = a + 6; p[0] && p[1]; p += 2) {
			char byte[3] = { p[0], p[1], 0 };
			cfg.token.push_back((uint8_t)strtoul(byte, NULL, 16));
		}
	}
	else if (!strchr(a, '=') && strchr(a, ':')) cfg.remote = a;
	else return false;
	return true;
}

int AutoDelay(unsigned pingMs, double fps)
{
	// GekkoNet's ping includes about one frame of waiting (17 ms on the same PC),
	// so that frame comes off before halving the round trip
	double frameMs = 1000.0 / fps;
	double ms = pingMs > frameMs ? pingMs - frameMs : 0;
	int d = (int)(ms / 2.0 / frameMs + 0.5);
	return d < 1 ? 1 : d > 8 ? 8 : d;
}

bool NetMatch::Start(const NetConfig& cfg, std::string* error)
{
	m_cfg = cfg;
	m_stats = NetStats();
	m_stats.delay = cfg.delay >= 0 ? cfg.delay : 2;   // automatic: 2 until the ping is known

	TransportConfig tc;
	tc.local = cfg.local;
	tc.port = cfg.port;
	tc.peer = cfg.remote;
	tc.relay = cfg.relay;
	tc.token = cfg.token;
	tc.relayOnly = cfg.relayOnly;
	tc.lagMs = cfg.lagMs; tc.jitterMs = cfg.jitterMs; tc.lossPct = cfg.lossPct;
	if (!m_transport.Open(tc, error) || !m_transport.Negotiate(error)) {
		m_transport.Close();
		return false;
	}
	g_transport = &m_transport;

	gekko_create(&m_session, GekkoGameSession);
	GekkoConfig gc{};
	gc.num_players = 2;
	gc.max_spectators = 0;
	gc.max_input_prediction_window = 8;
	gc.input_size = sizeof(uint16_t);
	gc.state_size = (unsigned int)(guasa_state_size() + sizeof(PauseState));   // the game, then the pause
	gc.desync_detection = true;
	gekko_start(m_session, &gc);
	gekko_net_adapter_set(m_session, &g_adapter);
	for (int p = 0; p < 2; p++) {
		if (p == cfg.local) {
			gekko_add_actor(m_session, GekkoLocalPlayer, nullptr);
			gekko_set_local_delay(m_session, p, (unsigned char)m_stats.delay);
		} else {
			GekkoNetAddress addr{ (void*)kPeer, (unsigned int)(sizeof(kPeer) - 1) };
			m_remoteHandle = gekko_add_actor(m_session, GekkoRemotePlayer, &addr);
		}
	}
	return true;
}

void NetMatch::SetDelay(int delay)
{
	if (delay < 0) delay = 0;
	if (delay > 8) delay = 8;
	m_stats.delay = delay;
	gekko_set_local_delay(m_session, m_cfg.local, (unsigned char)delay);
}

NetMatch::Status NetMatch::Step(uint16_t localInput, bool draw, bool* shown)
{
	if (shown) *shown = false;
	if (!m_endReason.empty()) return Ended;
	gekko_network_poll(m_session);

	int n = 0;
	GekkoSessionEvent** ev = gekko_session_events(m_session, &n);
	for (int i = 0; i < n; i++) {
		switch (ev[i]->type) {
		case GekkoSessionStarted: m_stats.started = true; m_startedAtFrame = m_stats.frame; break;
		case GekkoPlayerDisconnected: m_endReason = "el otro jugador se desconecto"; break;
		case GekkoDesyncDetected:
			if (m_stats.desyncs++ == 0) m_stats.firstDesync = ev[i]->data.desynced.frame;
			break;
		default: break;
		}
	}
	if (!m_endReason.empty()) return Ended;

	gekko_add_local_input(m_session, m_cfg.local, &localInput);
	GekkoGameEvent** up = gekko_update_session(m_session, &n);
	for (int i = 0; i < n; i++) {
		GekkoGameEvent* e = up[i];
		if (e->type == GekkoSaveEvent) {
			size_t len = guasa_save(e->data.save.state, guasa_state_size());
			memcpy(e->data.save.state + len, &m_pause, sizeof(PauseState));
			len += sizeof(PauseState);
			*e->data.save.state_len = (unsigned int)len;
			*e->data.save.checksum = (unsigned int)guasa_xxh64(e->data.save.state, len);
		} else if (e->type == GekkoLoadEvent) {
			unsigned int len = e->data.load.state_len - (unsigned int)sizeof(PauseState);
			guasa_load(e->data.load.state, len);
			memcpy(&m_pause, e->data.load.state + len, sizeof(PauseState));
		} else if (e->type == GekkoAdvanceEvent) {
			const uint16_t* raw = (const uint16_t*)e->data.adv.inputs;
			uint16_t in[2] = { (uint16_t)(raw[0] & ~kPauseBit), (uint16_t)(raw[1] & ~kPauseBit) };
			bool rolling = e->data.adv.rolling_back;
			// the replay: this frame's buttons (again, if a rollback redoes it)
			if (m_recordBase < 0) m_recordBase = e->data.adv.frame;
			size_t at = (size_t)(e->data.adv.frame - m_recordBase);
			if (e->data.adv.frame >= m_recordBase) {
				if (m_record.size() <= at) m_record.resize(at + 1, 0);
				m_record[at] = raw[0] | (uint32_t)raw[1] << 16;
			}
			// paused: the game doesn't move this frame (the same on both PCs)
			if (PauseRule(m_pause, m_cfg.pause, raw)) { m_stats.frame = e->data.adv.frame; if (shown && !rolling) *shown = true; continue; }
			// draw only frames that are shown; drawing doesn't change the state
			guasa_run_frame(in, draw && !rolling);
			if (rolling) { m_stats.resimulated++; m_run++; continue; }
			if (m_run) {
				m_stats.rollbacks++;
				m_stats.lastRollback = m_run;
				if (m_run > m_stats.maxRollback) m_stats.maxRollback = m_run;
				m_run = 0;
			}
			m_stats.frame = e->data.adv.frame;
			m_lastInputs[0] = in[0];
			m_lastInputs[1] = in[1];
			if (shown) *shown = true;
		}
	}

	if (m_stats.started && m_remoteHandle >= 0 && m_stats.frame % 30 == 0) {
		GekkoNetworkStats ns{};
		gekko_network_stats(m_session, m_remoteHandle, &ns);
		m_stats.ping = ns.last_ping;
		m_stats.jitter = ns.jitter;
		// Automatic delay: set once, after 2 seconds of ping samples (still at character select).
		if (m_cfg.delay < 0 && !m_autoDelaySet && m_stats.frame - m_startedAtFrame > 120 && ns.avg_ping > 0) {
			SetDelay(AutoDelay((unsigned)(ns.avg_ping + 0.5f), guasa_fps()));
			m_autoDelaySet = true;
		}
	}
	m_stats.framesAhead = gekko_frames_ahead(m_session);
	return m_stats.started ? Playing : Waiting;
}

bool PauseRule(PauseState& p, bool allowed, const uint16_t in[2])
{
	const int fps = (int)(guasa_fps() + 0.5);
	bool asked[2] = { (in[0] & kPauseBit) != 0, (in[1] & kPauseBit) != 0 };
	switch (p.phase) {
	case 0:
		if (!allowed || !(asked[0] || asked[1])) return false;
		p = { 1, asked[0] ? 0 : 1, 0 };
		return true;
	case 1:
		if (++p.frames >= 5 * 60 * fps) p = { 2, p.by, 10 * fps };   // 5 minutes: 10 s and on
		else if ((asked[0] || asked[1]) && p.frames > fps / 2) p = { 2, p.by, 3 * fps };   // asked to go on: 3 s
		return true;
	default:
		if (--p.frames <= 0) p = PauseState();
		return true;
	}
}

Replay NetMatch::Recording() const
{
	Replay r;
	r.pause = m_cfg.pause;
	r.frames = m_record;
	return r;
}

bool ReplayFrame(const Replay& rep, size_t i, PauseState& ps, bool draw)
{
	uint16_t raw[2] = { (uint16_t)rep.frames[i], (uint16_t)(rep.frames[i] >> 16) };
	if (PauseRule(ps, rep.pause, raw)) return false;
	uint16_t in[2] = { (uint16_t)(raw[0] & ~kPauseBit), (uint16_t)(raw[1] & ~kPauseBit) };
	guasa_run_frame(in, draw);
	return true;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string FramesToBase64(const uint32_t* frames, size_t n)
{
	std::vector<uint8_t> b;
	for (size_t i = 0; i < n; i++) for (int k = 0; k < 4; k++) b.push_back((uint8_t)(frames[i] >> (8 * k)));
	std::string out;
	for (size_t i = 0; i < b.size(); i += 3) {
		uint32_t v = b[i] << 16 | (i + 1 < b.size() ? b[i + 1] << 8 : 0) | (i + 2 < b.size() ? b[i + 2] : 0);
		out += kB64[v >> 18 & 63];
		out += kB64[v >> 12 & 63];
		out += i + 1 < b.size() ? kB64[v >> 6 & 63] : '=';
		out += i + 2 < b.size() ? kB64[v & 63] : '=';
	}
	return out;
}

bool FramesFromBase64(const std::string& text, std::vector<uint32_t>& out)
{
	std::vector<uint8_t> b;
	uint32_t v = 0;
	int bits = 0;
	for (char c : text) {
		if (c == '=') break;
		const char* p = strchr(kB64, c);
		if (!p || !c) return false;
		v = v << 6 | (uint32_t)(p - kB64);
		bits += 6;
		if (bits >= 8) { bits -= 8; b.push_back((uint8_t)(v >> bits)); }
	}
	if (b.size() % 4) return false;
	for (size_t i = 0; i < b.size(); i += 4) out.push_back(b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24);
	return true;
}

static void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }

bool SaveReplay(const std::string& path, const Replay& r)
{
	std::vector<uint8_t> b = { 'G', 'U', 'A', 'S', 'A', 'R', 'E', 'P' };
	Put16(b, 1);
	Put16(b, r.pause ? 1 : 0);
	uint32_t n = (uint32_t)r.frames.size();
	Put16(b, (uint16_t)n);
	Put16(b, (uint16_t)(n >> 16));
	for (uint32_t f : r.frames) { Put16(b, (uint16_t)f); Put16(b, (uint16_t)(f >> 16)); }
	FILE* fp = fopen(path.c_str(), "wb");
	if (!fp) return false;
	bool ok = fwrite(b.data(), 1, b.size(), fp) == b.size();
	return fclose(fp) == 0 && ok;
}

bool LoadReplay(const std::string& path, Replay& r, std::string* error)
{
	FILE* fp = fopen(path.c_str(), "rb");
	if (!fp) { if (error) *error = "no se pudo abrir la repeticion"; return false; }
	std::vector<uint8_t> b;
	uint8_t chunk[65536];
	size_t got;
	while ((got = fread(chunk, 1, sizeof(chunk), fp)) > 0) b.insert(b.end(), chunk, chunk + got);
	fclose(fp);
	auto u16 = [&](size_t at) { return (uint32_t)(b[at] | b[at + 1] << 8); };
	if (b.size() < 16 || memcmp(b.data(), "GUASAREP", 8) || u16(8) != 1) { if (error) *error = "no es una repeticion de La Guasabara"; return false; }
	uint32_t n = u16(12) | u16(14) << 16;
	if (b.size() != 16 + (size_t)n * 4) { if (error) *error = "la repeticion esta incompleta"; return false; }
	r.pause = (u16(10) & 1) != 0;
	r.frames.resize(n);
	for (uint32_t i = 0; i < n; i++) r.frames[i] = u16(16 + i * 4) | u16(18 + i * 4) << 16;
	return true;
}

double NetMatch::PaceFactor() const
{
	return m_stats.framesAhead > 0.5f ? 1.016 : 1.0;
}

void NetMatch::Stop()
{
	if (!m_session) return;
	gekko_disconnect_actor(m_session, m_cfg.local);   // tells the other side we left
	for (int i = 0; i < 10; i++) {
		gekko_network_poll(m_session);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	gekko_destroy(&m_session);
	m_session = nullptr;
	m_transport.Close();
	g_transport = nullptr;
}
