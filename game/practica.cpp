// Practice mode: see practica.h.
#include "practica.h"
#include <stdlib.h>
#include "guasa_core.h"

static const uint16_t kButtons = GUASA_LP | GUASA_MP | GUASA_HP | GUASA_LK | GUASA_MK | GUASA_HK;

const char* StanceName(int s)
{
	static const char* names[kStances] = { "de pie", "agachado", "salta", "bloquea siempre", "bloquea abajo",
		"bloquea tras el 1er golpe" };
	return s >= 0 && s < kStances ? names[s] : "";
}

const char* ReactionName(int r)
{
	static const char* names[kReactions] = { "nada", "la grabacion", "salta atras", "barrida", "golpe fuerte" };
	return r >= 0 && r < kReactions ? names[r] : "";
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

static uint32_t ReadLong(uint32_t a)
{
	return (uint32_t)ReadWord(a) << 16 | ReadWord(a + 2);
}

// One player's frame, by the rule worked out with core_test frames (±1 frame:
// the game sometimes runs two ticks in one frame).
FrameKind FrameMeter::Classify(int p)
{
	const uint32_t me = p ? kP2 : kP1, other = p ? kP1 : kP2;
	uint8_t st = guasa_read_ram(me + 3), sub = guasa_read_ram(me + 4);
	if (m_stop[p]) return kFrozen;   // hitstop: the frame after a nonzero counter doesn't move
	if (guasa_read_ram(me + 0x63) || st == 0x14) return kThrow;
	if (st == 0x0A || st == 0x0C || (st == 0x04 && sub == 0x06)) {   // attack, special, jump attack
		if (st == 0x0A && guasa_read_ram(other + 0x63)) return kThrow;   // the thrower
		if (st != 0x04 && sub == 0) return kStartup;   // its first tick: the animation isn't set yet
		uint32_t anim = ReadLong(me + 0x1A);
		if (guasa_read_mem(anim + 0x0C)) return kActive;
		if (guasa_read_mem(anim + 0x16) == 1) return kRecovery;
		if (st == 0x0C)   // a fireball out: the player is recovering
			for (uint32_t o = 0xFF9C22; o < 0xFF9C22 + 8 * 0xC0; o += 0xC0)
				if (guasa_read_ram(o) == 1 && ReadWord(o + 0x26) == (me & 0xFFFF)) return kRecovery;
		return kStartup;
	}
	if (st == 0x0E) {
		uint8_t how = guasa_read_ram(me + 0x4B);
		return how == 0x06 || how == 0x08 ? kBlockstun : sub == 0x02 ? kHitstun : kDown;
	}
	if (st == 0x08 && m_state[p] == 0x0E) return kBlockstun;   // one more frame after blocking
	if (st == 0x04) return sub == 0x04 && m_state[p] == 0x04 && m_sub[p] == 0x04 ? kFree : kAir;   // landing: free from its 2nd frame
	return kFree;
}

void ComboCounter::Update()
{
	int life = (int16_t)ReadWord(kP2 + kLife);
	if (life < 0) life = 0;   // a K.O. can take it under 0
	uint8_t st = guasa_read_ram(kP2 + 3), how = guasa_read_ram(kP2 + 0x4B);
	bool blocking = (st == 0x0E && (how == 0x06 || how == 0x08)) || st == 0x08;
	bool caught = (st == 0x0E && !blocking) || st == 0x14 || guasa_read_ram(kP2 + 0x63);
	if (life < m_life && !blocking) {   // a hit
		if (!live) { hits = damage = 0; live = true; }
		hits++;
		damage += m_life - life;
		if (damage > bestDamage || (damage == bestDamage && hits > bestHits)) { bestHits = hits; bestDamage = damage; }
	} else if (live && !caught) {   // free again: the combo is over
		live = false;
		shownFor = 180;
	}
	if (!live && shownFor > 0) shownFor--;
	m_life = life;
}

void ComboCounter::Reset()
{
	if (live) { live = false; shownFor = 180; }   // the one going on stays on screen a while
	int life = (int16_t)ReadWord(kP2 + kLife);
	m_life = life < 0 ? 0 : life;
}

static int16_t S16(uint32_t a) { return (int16_t)(guasa_read_mem(a) << 8 | guasa_read_mem(a + 1)); }
static uint32_t U32(uint32_t a) { return (uint32_t)(uint16_t)S16(a) << 16 | (uint16_t)S16(a + 2); }

std::vector<Box> ComputeBoxes()
{
	std::vector<Box> out;
	const int left = S16(0xFF8ED4), top = S16(0xFF8ED8);
	auto object = [&](uint32_t base, bool player) {
		int sx = S16(base + 0x06) - left, sy = 224 - (S16(base + 0x0A) - 15) + top;
		bool flip = guasa_read_mem(base + 0x12) == 1;
		uint32_t anim = U32(base + 0x1A), tab = U32(base + 0x34);
		if (!anim || !tab || anim >= 0xFF0000 || tab >= 0xFF0000) return;
		// which table offset, which id byte in the animation frame, entry size, kind
		struct { int tabOff, idAt, size; BoxKind kind; } kinds[] = {
			{ 8, 0x0D, 4, player ? kBoxPush : kBoxHurt },   // on a fireball: where it can be hit
			{ 0, 0x08, 4, kBoxHurt }, { 2, 0x09, 4, kBoxHurt }, { 4, 0x0A, 4, kBoxHurt },   // head, body, legs
			{ 6, 0x0C, 16, kBoxAttack },
		};
		for (auto& k : kinds) {
			if (k.kind == kBoxAttack && player && guasa_read_mem(base + 0x03) == 0) continue;
			int id = guasa_read_mem(anim + k.idAt);
			if (!id) continue;
			uint32_t e = tab + S16(tab + k.tabOff) + id * k.size;
			int dx = (int8_t)guasa_read_mem(e), dy = (int8_t)guasa_read_mem(e + 1);
			int rx = guasa_read_mem(e + 2), ry = guasa_read_mem(e + 3);
			if (k.kind == kBoxAttack && player && guasa_read_mem(e + 5) >= 0x80) dx = -(int)guasa_read_mem(e + 5);
			if (!rx || !ry) continue;
			int cx = sx + (flip ? -dx : dx), cy = sy - dy;
			out.push_back({ (int16_t)(cx - rx), (int16_t)(cy - ry), (int16_t)(cx + rx), (int16_t)(cy + ry), k.kind });
		}
	};
	for (uint32_t p : { kP1, kP2 }) if (guasa_read_mem(p)) object(p, true);
	for (int i = 0; i < 8; i++) {   // fireballs
		uint32_t base = 0xFF97A2 + 0xC0 * i;
		if ((uint16_t)S16(base) == 0x0101) object(base, false);
	}
	return out;
}

void FrameMeter::Update()
{
	FrameKind k[2] = { Classify(0), Classify(1) };
	for (int p = 0; p < 2; p++) {
		uint32_t me = p ? kP2 : kP1;
		m_state[p] = guasa_read_ram(me + 3);
		m_sub[p] = guasa_read_ram(me + 4);
		m_stop[p] = guasa_read_ram(me + 0x47);
	}
	if (k[0] == kFrozen && k[1] == kFrozen) return;   // hitstop isn't counted, as in SFV
	auto busy = [](FrameKind f) { return f != kFree && f != kAir; };
	if (done) {   // a new exchange starts when someone attacks or is hit
		if (!busy(k[0]) && !busy(k[1])) return;
		row[0].clear(); row[1].clear();
		done = false;
	}
	row[0].push_back(k[0]);
	row[1].push_back(k[1]);
	if ((k[0] == kFree && k[1] == kFree) || (int)row[0].size() >= kMax) Finish();
}

void FrameMeter::Finish()
{
	done = true;
	int freeAt[2];
	bool took[2];
	for (int p = 0; p < 2; p++) {
		int i = (int)row[p].size() - 1;
		while (i >= 0 && row[p][i] == kFree) i--;
		freeAt[p] = i + 1;
		took[p] = false;
		for (uint8_t f : row[p]) took[p] |= f != kFree && f != kAir;
	}
	bool over = row[0].back() == kFree && row[1].back() == kFree;
	hasAdvantage = over && took[0] && took[1];
	advantage = freeAt[1] - freeAt[0];
	// the first attack: its startup (frame of the first active one), active and recovery
	who = -1;
	for (size_t i = 0; i < row[0].size() && who < 0; i++)
		for (int p = 0; p < 2 && who < 0; p++)
			if (row[p][i] == kStartup || row[p][i] == kActive) who = p;
	startup = active = recovery = 0;
	if (who < 0) return;
	bool hit = false;
	for (uint8_t f : row[who]) {
		if (f == kActive) { active++; hit = true; }
		else if (f == kStartup && !hit) startup++;
		else if (f == kRecovery) recovery++;
	}
	if (active) startup++;   // counted as the frame the attack can first hit
}

void FrameMeter::Reset()
{
	done = true;
	row[0].clear(); row[1].clear();
	for (int p = 0; p < 2; p++) {
		uint32_t me = p ? kP2 : kP1;
		m_state[p] = guasa_read_ram(me + 3);
		m_sub[p] = guasa_read_ram(me + 4);
		m_stop[p] = 0;
	}
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
	if (!m_seq.empty()) {   // a reversal on its way
		int k = m_seqFrame++ - m_seqStart;
		if (k >= (int)m_seq.size()) m_seq.clear();
		else if (k >= 0) return m_seq[k];
		else return 0;
	}
	if (replay && !tape.empty()) {
		uint16_t v = tape[tapePos++ % tape.size()];
		return RivalOnRight() == m_tapeOnRight ? v : Mirror(v);
	}
	// Holding back only while the player attacks (or a fireball is out): held all
	// the time, back is walking away, and the rival would go off to the corner.
	uint16_t away = UnderAttack() ? (RivalOnRight() ? GUASA_RIGHT : GUASA_LEFT) : 0;
	switch (stance) {
	case kCrouch: return GUASA_DOWN;
	case kJump: return GUASA_UP;
	case kBlock: return away;
	case kBlockLow: return GUASA_DOWN | away;
	case kBlockAfterHit: return m_blockFor > 0 ? GUASA_DOWN | away : 0;
	default: return 0;
	}
}

// Knocked down (or thrown), or blocking: the moments a reversal follows.
Practice::Moment Practice::RivalMoment() const
{
	uint8_t st = guasa_read_ram(kP2 + 3), sub = guasa_read_ram(kP2 + 4), how = guasa_read_ram(kP2 + 0x4B);
	if (st == 0x0E && (how == 0x06 || how == 0x08)) return kBlockingNow;
	if (st == 0x14 || guasa_read_ram(kP2 + 0x63) || (st == 0x0E && sub != 0x02)) return kDownNow;
	return kNone;
}

// The rival's first free frame, found by playing ahead from a copy of the state
// (nobody pressing anything), then the reversal's buttons placed so that its
// button comes 1 frame before it (the game keeps it, and does the move on that
// first frame).
void Practice::PlanReversal(int what)
{
	m_seq.clear();
	std::vector<uint8_t> now(guasa_state_size());
	guasa_save(now.data(), now.size());
	int free = -1;
	for (int i = 0; i < 240 && free < 0; i++) {
		uint16_t none[2] = { 0, 0 };
		guasa_run_frame(none, 0);
		uint8_t st = guasa_read_ram(kP2 + 3);
		if (st != 0x0E && st != 0x14 && !guasa_read_ram(kP2 + 0x63)) free = i;
	}
	guasa_load(now.data(), now.size());
	if (free < 0) return;
	const uint16_t back = RivalOnRight() ? GUASA_RIGHT : GUASA_LEFT;
	std::vector<uint16_t> seq;
	int anchor = 0;   // the frame of seq that must come 1 frame before the first free one
	switch (what) {
	case kReactTape: {
		if (tape.empty()) return;
		bool flip = RivalOnRight() != m_tapeOnRight;
		size_t first = 0, button = tape.size();
		while (first < tape.size() && !tape[first]) first++;   // what was recorded before moving: nothing
		for (size_t i = first; i < tape.size() && button == tape.size(); i++) if (tape[i] & kButtons) button = i;
		if (first == tape.size()) return;
		if (button == tape.size()) button = first;   // only moving (a jump, a dash): from its first frame
		if (button - first > 20) first = button - 20;   // a special's motion is shorter than that
		for (size_t i = first; i < tape.size(); i++) seq.push_back(flip ? Mirror(tape[i]) : tape[i]);
		anchor = (int)(button - first);
		break;
	}
	case kReactJumpBack: seq.assign(4, (uint16_t)(GUASA_UP | back)); break;
	case kReactSweep: seq.assign(4, (uint16_t)(GUASA_DOWN | GUASA_HK)); break;
	case kReactHeavy: seq.assign(4, (uint16_t)GUASA_HP); break;
	default: return;
	}
	m_seq = seq;
	m_seqFrame = 0;
	m_seqStart = free - 1 - anchor;   // can be under 0: the start of the motion is lost, the button still comes in time
}

// The player is attacking (state 0A normal, 0C special, or a jump attack), or a fireball is out.
bool Practice::UnderAttack() const
{
	uint8_t st = guasa_read_ram(kP1 + 3), sub = guasa_read_ram(kP1 + 4);
	if (st == 0x0A || st == 0x0C || (st == 0x04 && sub == 0x06)) return true;
	for (uint32_t o = 0xFF97A2; o < 0xFF97A2 + 8 * 0xC0; o += 0xC0)
		if (guasa_read_ram(o) == 0x01 && guasa_read_ram(o + 1) == 0x01) return true;
	return false;
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
	// before the fight: the stage the player wants (the game reads it when the fight loads)
	if (phase != Fight && stage != -1) { guasa_write_ram(kStage, 0); guasa_write_ram(kStage + 1, stage == kOwnStage ? kOwnStageBase : (uint8_t)stage); }
	// From any phase: if the select screen's timer ran out, the game picked the
	// characters itself and the fight starts without the player's picks.
	bool started = phase != Fight && clock == 0x98 && m_lastClock == 0x99;
	m_lastClock = clock;
	if (started) {
		phase = Fight;
		if (freezeClock) guasa_write_ram(kClock, 0x99);
		if (m_spot.empty()) SavePosition();   // "volver a la posición" works from the first second
	}
	// the boxes of this frame; the picture shows them two frames from now
	m_boxes.pop_front();
	m_boxes.push_back(phase == Fight && (showBoxes || showPush) ? ComputeBoxes() : std::vector<Box>());
	if (started) { combo = ComboCounter(); combo.Reset(); }
	if (phase == Fight) {
		meter.Update();
		combo.Update();
		// a reversal: planned when the rival is knocked down or starts blocking
		// (again on each new hit or block, which changes when it gets free)
		Moment m = RivalMoment();
		uint8_t stop = guasa_read_ram(kP2 + 0x47);   // hitstop: set on every hit and every block
		bool fresh = m != kNone && (m != m_moment || (stop && !m_stop2));
		m_stop2 = stop;
		if (fresh && !controlRival && rec == NoRec) {
			int what = m == kDownNow ? onWake : onBlock;
			if (what != kReactNothing) PlanReversal(what);
		}
		m_moment = m;
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
			// The game keeps life in three places; writing only the first one, it sees
			// them differ and takes it for a new hit (the rival fell down again).
			if (refillLife && life > 0 && life < kFullLife && m_still[p] >= 60) {
				for (uint32_t copy : { kLife, kLifeCopy, kLifeBar }) WriteWord((p ? kP2 : kP1) + copy, kFullLife);
				m_lastLife[p] = kFullLife;
			}
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
	meter.Reset();
	combo.Reset();
	m_seq.clear();   // a reversal planned before going back: not anymore
	m_moment = kNone;
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
	meter.Reset();
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
	if (onWake) s += std::string("   al levantarse: ") + ReactionName(onWake);
	if (onBlock) s += std::string("   tras bloquear: ") + ReactionName(onBlock);
	if (delay) s += "   delay " + std::to_string(delay);
	return s;
}
