// Practice mode: see practica.h.
#include "practica.h"
#include "guasa_core.h"

static const uint16_t kButtons = GUASA_LP | GUASA_MP | GUASA_HP | GUASA_LK | GUASA_MK | GUASA_HK;

const char* StanceName(int s)
{
	static const char* names[kStances] = { "de pie", "agachado", "salta", "bloquea siempre", "bloquea abajo",
		"bloquea tras el 1er golpe" };
	return s >= 0 && s < kStances ? names[s] : "";
}

uint16_t ReadWord(uint32_t a)
{
	return (uint16_t)(guasa_read_ram(a) << 8 | guasa_read_ram(a + 1));
}

static void WriteWord(uint32_t a, uint16_t v)
{
	guasa_write_ram(a, (uint8_t)(v >> 8));
	guasa_write_ram(a + 1, (uint8_t)v);
}

// Left and right swapped: a recording made from the other side.
static uint16_t Mirror(uint16_t in)
{
	uint16_t lr = in & (GUASA_LEFT | GUASA_RIGHT);
	in &= ~(GUASA_LEFT | GUASA_RIGHT);
	if (lr & GUASA_LEFT) in |= GUASA_RIGHT;
	if (lr & GUASA_RIGHT) in |= GUASA_LEFT;
	return in;
}

void Practice::Start()
{
	m_select.resize(guasa_state_size());
	guasa_save(m_select.data(), m_select.size());
	phase = PickMine;
}

bool Practice::RivalOnRight() const
{
	return ReadWord(kP2 + kX) > ReadWord(kP1 + kX);
}

uint16_t Practice::Rival(uint16_t)
{
	if (replay && !tape.empty()) {
		uint16_t v = tape[tapePos++ % tape.size()];
		return RivalOnRight() == m_tapeOnRight ? v : Mirror(v);
	}
	uint16_t away = RivalOnRight() ? GUASA_RIGHT : GUASA_LEFT;
	switch (stance) {
	case kCrouch: return GUASA_DOWN;
	case kJump: return GUASA_UP;
	case kBlock: return away;
	case kBlockLow: return GUASA_DOWN | away;
	case kBlockAfterHit: return m_blockFor > 0 ? GUASA_DOWN | away : 0;
	default: return 0;
	}
}

void Practice::Inputs(uint16_t local, int second, uint16_t out[2])
{
	uint16_t pressed = local & ~m_lastLocal;
	m_lastLocal = local;
	out[0] = out[1] = 0;
	// At character select, what was already held (the button that picked the
	// player's character, or the menu's) doesn't reach the game until it's
	// released: otherwise the rival's cursor would pick at once (Ken).
	if (m_grabHeld) { m_held = local; m_grabHeld = false; }
	m_held &= local;
	uint16_t fresh = local & ~m_held;
	if (phase == PickMine) {
		out[0] = fresh;
		if (pressed & kButtons) { phase = PickRival; m_held = local; }
		return;
	}
	if (phase == PickRival) {
		out[1] = fresh;
		if (pressed & kButtons) phase = WaitFight;
		return;
	}
	if (controlRival) {
		out[1] = local;
		if (rec == Recording) tape.push_back(local);
		return;
	}
	// the player's buttons, late by `delay` frames like online
	m_late.push_back(local);
	out[0] = 0;
	while ((int)m_late.size() > delay) { out[0] = m_late.front(); m_late.pop_front(); }
	out[1] = second >= 0 ? (uint16_t)second : Rival(out[0]);
}

void Practice::AfterFrame(const uint16_t used[2])
{
	// The clock shows 99 already during the round's intro, before anyone can move;
	// its first tick (99 -> 98) is when the fight really starts.
	uint8_t clock = guasa_read_ram(kClock);
	bool started = phase == WaitFight && clock == 0x98 && m_lastClock == 0x99;
	m_lastClock = clock;
	if (started) {
		phase = Fight;
		if (freezeClock) guasa_write_ram(kClock, 0x99);
		if (m_spot.empty()) SavePosition();   // "volver a la posición" works from the first second
	}
	if (phase == Fight) {
		if (freezeClock) guasa_write_ram(kClock, 0x99);
		if (fullSuper) { guasa_write_ram(kP1 + kSuper, kFullSuper); guasa_write_ram(kP2 + kSuper, kFullSuper); }
		for (int p = 0; p < 2; p++) {
			uint32_t at = (p ? kP2 : kP1) + kLife;
			int life = (int16_t)ReadWord(at);
			if (p == 1) {
				if (life < m_lastLife[1]) m_blockFor = 40;   // just got hit
				else if (m_blockFor > 0) m_blockFor--;
			}
			m_still[p] = life == m_lastLife[p] ? m_still[p] + 1 : 0;
			m_lastLife[p] = life;
			// a second without being hit: full again (not on a K.O., which ends the round)
			if (refillLife && life > 0 && life < kFullLife && m_still[p] >= 60) { WriteWord(at, kFullLife); m_lastLife[p] = kFullLife; }
		}
	}
	for (int p = 0; p < 2; p++) {
		uint16_t v = used[p] & ~(GUASA_START | GUASA_COIN);
		auto& h = history[p];
		if (h.empty() || h.front().in != v) h.push_front({ v, 1 });
		else if (h.front().frames < 999) h.front().frames++;
		while (h.size() > 18) h.pop_back();
	}
}

void Practice::ToggleRecord()
{
	if (rec == NoRec) {
		rec = Recording;
		tape.clear();
		tapePos = 0;
		replay = false;
		controlRival = true;
		m_tapeOnRight = RivalOnRight();
	} else {
		rec = NoRec;
		controlRival = false;
		tapePos = 0;
		replay = !tape.empty();   // right away: the rival does what was just recorded
	}
}

void Practice::SavePosition()
{
	m_spot.resize(guasa_state_size());
	guasa_save(m_spot.data(), m_spot.size());
}

bool Practice::LoadPosition()
{
	if (m_spot.empty()) return false;
	guasa_load(m_spot.data(), m_spot.size());
	for (int p = 0; p < 2; p++) { m_lastLife[p] = (int16_t)ReadWord((p ? kP2 : kP1) + kLife); m_still[p] = 0; }
	m_late.clear();
	m_blockFor = 0;
	tapePos = 0;   // the recording starts over with the position
	return true;
}

void Practice::ChangeCharacters()
{
	if (m_select.empty()) return;
	guasa_load(m_select.data(), m_select.size());
	phase = PickMine;
	m_grabHeld = true;
	m_spot.clear();
	tape.clear();
	rec = NoRec;
	replay = controlRival = false;
	m_late.clear();
	m_lastClock = 0;
	for (auto& h : history) h.clear();
}

std::string Practice::Status() const
{
	if (phase == PickMine) return "Elige tu personaje";
	if (phase == PickRival) return "Ahora elige el del rival (con el mismo control)";
	if (phase == WaitFight) return "PRACTICA";
	std::string s = "PRACTICA   Rival: ";
	if (rec == Recording) s += "GRABANDO (tu lo mueves)";
	else if (controlRival) s += "lo mueves tu";
	else if (replay && !tape.empty()) s += "repite la grabacion";
	else s += StanceName(stance);
	if (delay) s += "   delay " + std::to_string(delay);
	return s;
}
