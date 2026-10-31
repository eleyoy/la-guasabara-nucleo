// Who won each game, read from the game's RAM (PLAN.md appendix C, Fightcade's
// detector): the round wins of P1 and P2 are at FF87DE and FF8BDE, and the game
// is over when one reaches 2. Checked with the core on 10,000 frames of a real
// match (game won by P1 2-1 on frame 9003; both back to 0 when the next starts).
#pragma once
#include <stdint.h>
#include "guasa_core.h"

class ResultWatcher {
public:
	// Call once per shown frame. Returns 1 or 2 the first time a game is over
	// (after holding for 30 frames, longer than any rollback can undo), else 0.
	int Update()
	{
		int w1 = guasa_read_ram(0xFF87DE), w2 = guasa_read_ram(0xFF8BDE);
		int winner = w1 == 2 && w2 < 2 ? 1 : w2 == 2 && w1 < 2 ? 2 : 0;
		if (winner == 0) {
			m_held = 0;
			if (w1 < 2 && w2 < 2) m_reported = false;   // a new game: ready for its result
			return 0;
		}
		if (m_reported) return 0;
		if (winner != m_candidate) { m_candidate = winner; m_held = 0; }
		if (++m_held < kHold) return 0;
		m_reported = true;
		return winner;
	}

private:
	static const int kHold = 30;
	int m_candidate = 0, m_held = 0;
	bool m_reported = false;
};
