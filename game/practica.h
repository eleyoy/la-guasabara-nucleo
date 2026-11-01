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
};
constexpr int kFullLife = 0x90, kFullSuper = 0x90;

enum Stance { kStand, kCrouch, kJump, kBlock, kBlockLow, kBlockAfterHit, kStances };
const char* StanceName(int s);

struct HistoryEntry { uint16_t in; int frames; };

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
	std::vector<uint8_t> m_select, m_spot;
	std::deque<uint16_t> m_late;
	uint16_t m_lastLocal = 0;
	uint16_t m_held = 0;            // held since the last pick: kept from the game until released
	bool m_grabHeld = false;
	int m_lastLife[2] = { kFullLife, kFullLife }, m_still[2] = { 0, 0 };
	int m_blockFor = 0;
	bool m_tapeOnRight = true;
	uint8_t m_lastClock = 0;
};

uint16_t ReadWord(uint32_t a);
