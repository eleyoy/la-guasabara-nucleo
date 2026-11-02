// Practice mode (PLAN.md section 4b): one player against a rival that does what
// they ask, on this PC only. No SDL here: the game window draws it and handles
// the keys, and "guasa-game practica-prueba" checks it without a window.
//
// What it does, frame by frame, on top of guasa-core:
// - the rival (player 2) stands, crouches, jumps, blocks, or plays back what the
//   player recorded while controlling it (turned around if the sides changed);
// - life comes back after a second without being hit, the clock stays at 99 and
//   the super meter can stay full (writing the game's RAM: never online);
// - one saved position to go back to, in memory only (never a file: players
//   don't get savestates);
// - the player's buttons can arrive a few frames late, like online;
// - a history of the buttons with how many frames each one lasted.
#pragma once
#include <stdint.h>
#include <deque>
#include <string>
#include <vector>

// RAM (appendix C of the plan; position and super meter found with core_test explore)
enum : uint32_t {
	kP1 = 0xFF844E, kP2 = 0xFF884E,   // each player's block
	kLife = 0x2A,                     // word; 0x90 full
	kLifeCopy = 0x2C, kLifeBar = 0x1BC,   // the same life, kept twice more (all three go together)
	kX = 0x06,                        // word; position on the stage
	kSuper = 0x39C,                   // byte; 0x90 full
	kClock = 0xFF8DCE,                // BCD; 0x99 during the round's intro, 0x98 once the fight runs
	kStage = 0xFFE18A,                // word: whose stage the fight is on (a character id; 0x0C Cammy)
};
constexpr int kFullLife = 0x90, kFullSuper = 0x90;

constexpr int kOwnStage = -2;
// Under practice's own stage the game's runs hidden: Vega's, one of the two of the 16
// that draw no loose sprites (no birds, barrels or dancers) over the grid.
constexpr uint8_t kOwnStageBase = 0x0B;

enum Stance { kStand, kCrouch, kJump, kBlock, kBlockLow, kBlockAfterHit, kStances };
const char* StanceName(int s);

struct HistoryEntry { uint16_t in; int frames; };

// The frame meter, like modern games: what each player does on every frame of an
// exchange (startup, active, recovery, hit or block stun...), and who can act
// first when it's over. Read from the game's RAM and its animation tables in ROM
// (found with core_test frames):
//   +0x03 state: 00 stand, 02 crouch, 04 jump, 08 guard, 0A attack, 0C special,
//         0E hit/block stun or knockdown, 14 thrown; +0x04 its sub-state
//   +0x1A pointer to the animation frame in ROM: +0x0C attack box (active),
//         +0x16 = 1 after the last active frame (recovery)
//   +0x47 hitstop counter, +0x4B stun type (06/08 = block), +0x63 held by a throw
// Hitboxes: every box of both players and their fireballs, in pixels of the
// 384x224 picture (found with core_test boxes; the recipe is the one of the
// hitbox viewer of Fightcade's training mode, checked on this core):
//   camera: left FF8ED4, top FF8ED8; object at sx = X(+0x06) - left,
//   sy = 224 - (Y(+0x0A) - 15) + top; +0x12 = 1 facing right; +0x1A animation
//   frame and +0x34 the character's box table, both in ROM. Each kind of box:
//   an id in the animation frame and a list in the table (dx, dy, half width,
//   half height). The picture is 2 frames behind RAM: the boxes are drawn late too.
enum BoxKind : uint8_t { kBoxHurt, kBoxAttack, kBoxPush };
struct Box { int16_t l, t, r, b; BoxKind kind; };
std::vector<struct Box> ComputeBoxes();

enum FrameKind : uint8_t { kFree, kStartup, kActive, kRecovery, kHitstun, kBlockstun, kDown, kAir, kThrow, kFrozen };

class FrameMeter {
public:
	static constexpr int kMax = 150;   // frames kept of one exchange
	std::vector<uint8_t> row[2];       // the exchange shown: one FrameKind per frame and player
	bool done = true;                  // it's over: the numbers below are final
	bool hasAdvantage = false;         // both took part (a hit or a block, not a whiff)
	int advantage = 0;                 // for player 1: + means they can act first
	int who = -1, startup = 0, active = 0, recovery = 0;   // the first attack in it and whose (0/1)
	void Update();                     // after every frame of the fight
	void Reset();                      // after loading a position: nothing pending
private:
	FrameKind Classify(int p);
	void Finish();
	uint8_t m_state[2] = {}, m_sub[2] = {}, m_stop[2] = {};   // last frame's
};

class Practice {
public:
	// Characters are picked first: the player's, then the rival's with the same control.
	enum Phase { PickMine, PickRival, WaitFight, Fight };
	enum Rec { NoRec, Recording };

	Phase phase = PickMine;
	int stance = kStand;
	bool controlRival = false;      // the player moves the rival (to record it, or to try it)
	Rec rec = NoRec;
	bool replay = false;            // the rival plays the recording over and over
	bool refillLife = true, freezeClock = true, fullSuper = false;
	int delay = 0;                  // frames the player's buttons arrive late (0-5)
	bool showHistory = true;
	bool showFrames = true;         // the frame meter
	int stage = 0x0C;               // the fight's stage, a character id (Cammy's by default); -1 = the game's choice;
	                                // kOwnStage = practice's own (a light grid; the game's stage underneath is hidden)
	bool showBoxes = true, showPush = false;   // hitboxes (hurt blue, attack red); pushboxes (green)
	// The boxes of the picture on screen now (it's 2 frames behind the game's RAM).
	const std::vector<Box>& ShownBoxes() const { return m_boxes.front(); }
	FrameMeter meter;
	std::vector<uint16_t> tape;     // the rival's recorded buttons
	size_t tapePos = 0;
	std::deque<HistoryEntry> history[2];

	// Right after the boot to character select: kept to change characters later.
	void Start();
	// The two players' buttons for this frame, from the player's control
	// (local) and a second control if there is one (second, -1 = none).
	void Inputs(uint16_t local, int second, uint16_t out[2]);
	// After guasa_run_frame: life, clock, super, the saved position at the start
	// of each fight, and the history.
	void AfterFrame(const uint16_t used[2]);

	void ToggleRecord();
	void SavePosition();
	bool LoadPosition();            // false if there is none yet
	void ChangeCharacters();
	bool HasPosition() const { return !m_spot.empty(); }
	std::string Status() const;     // one line for the top of the screen

private:
	uint16_t Rival(uint16_t p1Local);
	bool RivalOnRight() const;
	bool UnderAttack() const;
	std::vector<uint8_t> m_select, m_spot;
	std::deque<uint16_t> m_late;
	uint16_t m_lastLocal = 0;
	std::deque<std::vector<Box>> m_boxes = std::deque<std::vector<Box>>(3);   // the last 3 frames' boxes, oldest first
	uint16_t m_held = 0;            // held since the last pick: kept from the game until released
	bool m_grabHeld = false;
	int m_lastLife[2] = { kFullLife, kFullLife }, m_still[2] = { 0, 0 };
	int m_blockFor = 0;
	bool m_tapeOnRight = true;
	uint8_t m_lastClock = 0;
};

uint16_t ReadWord(uint32_t a);
