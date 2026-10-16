/* The fork's trimmed pause menu (its Extras, Save Game and Host Wireless rows
 * removed),
 * the part that is pure arithmetic.
 *
 * WHY THIS IS ITS OWN FILE. The pause menu took three attempts to get right and
 * the third one could not execute at all - the corrections were gated on an `lr`
 * comparison that could never be true, so two rounds of play-testing showed four
 * identical symptoms. What finally worked is now CONFIRMED BY SCREENSHOT, and the piece
 * most likely to be broken by a later edit is the layout arithmetic: which rows
 * are hidden, and where the survivors sit.
 *
 * That part needs no guest at all. Splitting it out means it can be tested, and
 * the rest of dispatch_overrides.cpp keeps the bus access.
 *
 * THE BUILDER'S OWN NUMBERS, read at 0x0208E980:
 *
 *     0208E984  MOV  r7, #0x28        Y starts at 40
 *     0208E9E0  MOV  r2, #0x80        X is 128 - already centred
 *     0208EA24  ADD  r7, r7, #0x19    +25 PER ROW, hidden or not
 *
 * That last line is the whole reason this exists: blanking three of six rows
 * left three 25-pixel holes, and a menu that reads as broken rather than short.
 */

#ifndef LSW2_PAUSELAYOUT_H
#define LSW2_PAUSELAYOUT_H

#include <cstdint>

constexpr uint32_t kLsw2PauseRowCount = 6u;

/* THE THREE ROWS THE FORK HIDES, BY POSITION. One copy, because there were two:
 * pauselayout.cpp called it kHiddenByIndex and dispatch_overrides.cpp called it
 * kPauseHiddenRows, both {1, 3, 4}, in different files.
 *
 * They agreed, which is the only reason it had not bitten - and this is the
 * fourth time this project has found one fact written twice, after the gear
 * applier's address, kCustomBase and the level-pin race claim. Here the two
 * copies serve DIFFERENT halves of the same feature: pauselayout.cpp's is the
 * cursor's answer before the builder has been round a row, and
 * dispatch_overrides.cpp's is the loop that clears each row's enabled byte to
 * close the TOUCH path. Letting those two drift would leave a row the cursor
 * skips still tappable, which is precisely the failure the pause menu showed
 * twice.
 *
 * POSITION IS THE FALLBACK, NOT THE RULE. Once the builder has been round a row
 * its string id is known and lsw2_pause_row_hidden() answers on the id instead,
 * which is how the rows are specified and what survives row 1's label changing
 * between "Host Wireless", "Shut Off Wireless" and "Exit Wireless Game". */
constexpr uint32_t kLsw2PauseHiddenRows[] = {1u, 3u, 4u};

/* WHERE A HIDDEN ROW IS DRAWN: below the bottom screen's 192 lines, so the
 * builder's own draw submits it and the rasteriser never shows it. Measured:
 * handing a hidden row its original slot instead left
 * "Shut Off Wireless" drawn over "Resume" on a hosting fork, because the
 * visible rows compact into exactly that slot. See lsw2_pause_row_y. */
constexpr uint32_t kLsw2PauseOffScreenY = 200u;
constexpr int kLsw2PauseHiddenRowCount = 3;
constexpr uint32_t kLsw2PauseFirstY = 0x28u;   /* MOV r7, #0x28     */
constexpr uint32_t kLsw2PausePitch = 0x19u;    /* ADD r7, r7, #0x19 */

/* THE HIDDEN ROWS, by their string ids rather than their positions.
 *
 * The ROM keeps no static row list: 0x0208E980 walks
 * [menu + 0xE8] rows, and 0x0208E874 picks between 0x03C "Host Wireless" and
 * 0x03D "Shut Off Wireless" for what is presumably one row. Hiding by index is
 * therefore right only in the state a screenshot happened to capture.
 *
 * The ids are unambiguous, and the rows to hide are specified by NAME -
 * Extras, Host Wireless and Save Game - so matching on id is the more faithful
 * reading as well as the more robust one. Both wireless labels are hidden,
 * which is what "hide Host Wireless" means when the row can also read "Shut
 * Off Wireless". */
constexpr uint32_t kLsw2PauseIdCustomize = 0x03Bu;
constexpr uint32_t kLsw2PauseIdHostWireless = 0x03Cu;
constexpr uint32_t kLsw2PauseIdShutOffWireless = 0x03Du;
/* THE THIRD LABEL, and the one that matters most. 0x0208E8F4 picks it when
 * `[0x027E02A8] & 0x40` - the multi-view bit - is set, which is exactly the
 * fork's arena. Hiding only 0x03C and 0x03D would show "Exit Wireless Game" in
 * the one place the fork actually runs. */
constexpr uint32_t kLsw2PauseIdExitWireless = 0x03Eu;
constexpr uint32_t kLsw2PauseIdExtras = 0x007u;
constexpr uint32_t kLsw2PauseIdSaveGame = 0x02Cu;
constexpr uint32_t kLsw2PauseIdQuitGame = 0x044u;

/* Whether a row carrying this string id is one of the three hidden rows. Pure. */
bool lsw2_pause_id_hidden(uint32_t string_id);

/* WHAT THE BUILDER IS ABOUT TO DRAW, recorded as it goes.
 *
 * The id is not stored anywhere the port can read later: the row records at
 * `menu + 0x98` are 12 bytes of hit-test rectangle (`+4` x, `+6` y, `+8` w,
 * `+0xA` h) and an active byte, written by 0x02064B10 - no id among them. The
 * ids live in a stack-local halfword array the loop indexes at 0x0208E9C0.
 *
 * But the port's existing hook sits one instruction later: it replaces
 * 0x02066708, called at 0x0208E9C8 with `MOV r0, r9` immediately before, so the
 * id is in R[0] every time the loop comes round. This records it against the
 * row index so the layout functions can ask by identity.
 *
 * Row 0 clears the table, because the loop restarts there every frame and a
 * menu that gained or lost a row must not keep the old answers. */
void lsw2_pause_note_row_id(uint32_t row, uint32_t string_id);

/* The three hidden rows. Uses the recorded id when the builder has been round this row,
 * and FALLS BACK to the index rule when it has not - which is what the cursor
 * and layout helpers need before the first frame of a fresh menu. */
bool lsw2_pause_row_hidden(uint32_t row);

/* Whether this row's identity is known yet. For the tests, and for a caller
 * that wants to know whether it is trusting an id or a position. */
bool lsw2_pause_row_id_known(uint32_t row);

/* Where a row should actually be drawn once the hidden ones are gone.
 *
 * Survivors keep the game's own pitch and are re-centred on the SAME midpoint
 * the full six occupied, so the block stays where the pause screen's art frames
 * it instead of drifting to the top.
 *
 * A hidden row keeps its original Y: it draws nothing, so where it goes does not
 * matter, and moving it would only risk it landing on a visible one. */
uint32_t lsw2_pause_row_y(uint32_t row);

/* Which visible row the cursor should move to, having landed on `from`.
 *
 * `last` is where the cursor came to rest BEFORE this press - the only direction
 * signal available. Storing the corrected value here instead is what broke
 * "moving up to Resume", because it made the signal a record of our own writes
 * rather than of the player's movement. */
uint32_t lsw2_pause_escape(uint32_t from, uint32_t last);

#endif /* LSW2_PAUSELAYOUT_H */
