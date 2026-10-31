#include "net.h"
#include <string.h>
#include <stdlib.h>
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
	gc.state_size = (unsigned int)guasa_state_size();
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
			*e->data.save.state_len = (unsigned int)len;
			*e->data.save.checksum = (unsigned int)guasa_xxh64(e->data.save.state, len);
		} else if (e->type == GekkoLoadEvent) {
			guasa_load(e->data.load.state, e->data.load.state_len);
		} else if (e->type == GekkoAdvanceEvent) {
			const uint16_t* in = (const uint16_t*)e->data.adv.inputs;
			bool rolling = e->data.adv.rolling_back;
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
