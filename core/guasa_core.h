/*
 * guasa-core: the league's narrow interface to the ssf2xjr1 core (plan, section 3).
 *
 * Level 1 (L1) wraps the pinned fightcade-fbneo CPS2 core. A later core of our
 * own (L2) only has to provide these same functions, and the rest of the app
 * never notices the change.
 *
 * Rules this interface keeps (plan, section 9):
 *   - QSound is rendered every frame, at one fixed rate for every player.
 *   - Saved states include the QSound resampler state (spike patch 0001).
 *   - The league's SOCD rule is applied to both players inside run_frame.
 *   - One game per process: init once, never re-init in the same process
 *     (the EEPROM keeps data across re-inits).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The fixed internal sound rate: Fightcade's default. */
#define GUASA_SOUND_RATE 44100

/* Input bits of one player, as sent over the network. */
enum {
    GUASA_UP = 1 << 0, GUASA_DOWN = 1 << 1, GUASA_LEFT = 1 << 2, GUASA_RIGHT = 1 << 3,
    GUASA_LP = 1 << 4, GUASA_MP = 1 << 5, GUASA_HP = 1 << 6,
    GUASA_LK = 1 << 7, GUASA_MK = 1 << 8, GUASA_HK = 1 << 9,
    GUASA_START = 1 << 10, GUASA_COIN = 1 << 11
};

/* Boots ssf2xjr1 from the player's own files in rom_dir (UTF-8): the zips as
 * they come (ssf2xjr1.zip plus ssf2t.zip, or Fightcade's ssf2xjr1.zip with
 * everything) or loose files, matched by CRC32. Factory settings, CPU at 100%,
 * netplay mode as in Fightcade. Returns 0 on success; on failure, message
 * (if not NULL) gets a short reason. */
int guasa_init(const char* rom_dir, char* message, size_t message_len);
void guasa_exit(void);

/* Checks rom_dir before playing, without starting the game. Returns how many
 * files are missing or wrong (0 = ready), or -1 if it couldn't check; report
 * gets one line per problem in plain Spanish ("Falta sfx.13m (va en
 * ssf2t.zip ...)"), or "Todo bien ..." when nothing is missing. */
int guasa_check_rom(const char* rom_dir, char* report, size_t report_len);

/* Frames per second of the game: 59.63. */
double guasa_fps(void);

/* Runs one frame. inputs[0] is P1, inputs[1] is P2 (GUASA_* bits). With
 * draw = 0 the picture isn't drawn, which doesn't change the state. */
void guasa_run_frame(const uint16_t inputs[2], int draw);

/* Saved state for rollback and replays. guasa_state_size() is the exact size;
 * guasa_save() returns the bytes written (0 if cap is too small). */
size_t guasa_state_size(void);
size_t guasa_save(uint8_t* buf, size_t cap);
int guasa_load(const uint8_t* buf, size_t len);

/* Hash of the whole saved state (what both machines compare every frame). */
uint64_t guasa_state_hash(void);
/* Hash of the 68000's work RAM only: what the game logic sees. It doesn't
 * depend on how a core lays out its state, so it is what L1 and L2 compare. */
uint64_t guasa_game_hash(void);

/* One byte of the 68000's work RAM, 0xFF0000-0xFFFFFF (appendix C). */
uint8_t guasa_read_ram(uint32_t address);
/* Changes one byte of it. Practice mode only (infinite life and time): never
 * online, where both machines must run exactly the same game. */
void guasa_write_ram(uint32_t address, uint8_t value);
/* One byte as the 68000's data reads see it: program ROM (0x000000 up, the
 * animation and hitbox tables) or work RAM; anything else reads 0. Read-only,
 * for practice-mode tools such as the frame meter. */
uint8_t guasa_read_mem(uint32_t address);

/* The last drawn frame: RGB565, width x height, pitch in bytes. */
const uint16_t* guasa_framebuffer(int* width, int* height, int* pitch);
/* This frame's sound: interleaved stereo 16-bit, *samples frames of it. */
const int16_t* guasa_audio(int* samples);

/* Leaves the game at character select with both players in, like the state
 * Fightcade loads before an online match: coins and starts with a fixed
 * input script. Every PC gets the same state; compare guasa_state_hash()
 * before playing. Returns the frame count it took, or -1. */
int guasa_boot_to_select(void);

/* The buttons the game actually reads for these: the league's rule for
 * opposite directions (left + right = neutral, up + down = up), which
 * guasa_run_frame applies to both players. For Test Game. */
uint16_t guasa_socd(uint16_t input);

/* Certification kit only: loads a decompressed FBNeo state file (the data
 * after the zlib header of a .fs, scanned with ACB_FULLSCAN). Returns 0 if
 * every byte was used. Players never load state files. */
int guasa_load_state_file(const uint8_t* buf, size_t len);

/* 64-bit XXH64 of a buffer (seed 0), used by the hashes above. */
uint64_t guasa_xxh64(const void* data, size_t len);

#ifdef __cplusplus
}
#endif
