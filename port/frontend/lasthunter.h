/* The fork's direct boot, and its "or their last-played Bounty Hunter".
 *
 * The fork places the player directly in the Mos Eisley Cantina arena lobby
 * map, as Greedo or as their last-played Bounty Hunter.
 *
 * So the fork has to REMEMBER which hunter the player was, across runs, and
 * come back as them. Greedo is the fallback, and he is the fallback because he
 * is entry 0 of the game's own roster table at 0x020FFA00 rather than because
 * the port picked a favourite.
 *
 * TWO HALVES.
 *
 *   remember   watch who the player is while they are in the arena lobby or a
 *              match, and keep the last hunter across runs.
 *   apply      ask the game to make them that hunter on arrival.
 *
 * THE APPLY HALF IS A BYTE, but not the obvious one. `actor+0x6BB` is written
 * by the playable-character constructor 0x02001D20 and overwriting it
 * afterwards would rename an actor the game had already built out of somebody
 * else's parts - the same mistake the customization auto-apply made and had to
 * abandon. The two obvious interception points around that constructor turned
 * out to be closed as well.
 *
 * The byte that works is a REQUEST the game polls: there is a per-player
 * pending-character field on the level root that a frame poller turns into the
 * game's own character change. So the port asks, and the guest acts, which is
 * the shape this project prefers and the reason the closed routes could stay
 * closed.
 */

#ifndef LSW2_LASTHUNTER_H
#define LSW2_LASTHUNTER_H

#include <cstdint>

/* THE GAME'S OWN ROSTER, from the table at 0x020FFA00, in the
 * table's order. Ten entries, and Greedo at index 0.
 *
 * Kept here as data rather than read from guest memory at every call: this is a
 * ROM constant, the address is known, and a membership test that needed a live
 * guest would answer "not a hunter" for every id whenever the game happened to
 * be between levels. */
constexpr int kLsw2HunterCount = 10;
extern const int kLsw2HunterIds[kLsw2HunterCount];

/* Greedo. Entry 0 of the table above, and the fork's default hunter. */
constexpr int kLsw2DefaultHunter = 20;

/* Is this character id one of the ten? Anything else - a story character, a
 * custom slot, a sentinel - is not a hunter and must never be remembered as
 * one, or the fork would boot the player in as Chewbacca. */
bool lsw2_is_bounty_hunter(int character_id);

/* The hunter's index in the roster table, or -1.
 *
 * THIS IS NOT A PAGE AND SLOT. The roster table's order and the Free Play
 * select screen's order are different orders over the same ten characters, and
 * assuming otherwise is a mistake an earlier reading made and this file
 * repeated - see
 * lsw2_hunter_page_slot() below. Use this only for questions about
 * 0x020FFA00 itself. */
int lsw2_hunter_index(int character_id);

/* THE FREE PLAY SELECT SCREEN'S OWN PAGES, from the table at 0x020EDDB2 that
 * 0x020687B4 indexes as `table[page * 8 + slot]`. Twelve pages of
 * eight, and the widget's own upper bound - written as 0x0C by its constructor
 * at 0x02068320 - agrees.
 *
 * Pages 6 and 7 are the Bounty Hunters, 8 + 2, and
 * they contain precisely the ten ids of the roster table - IN A DIFFERENT
 * ORDER. An earlier reading inferred that the pages followed the roster's order and
 * flagged the inference; the table says otherwise, and the split lands in a
 * different place than that inference predicted.
 *
 * Which is why this is a lookup and not arithmetic on lsw2_hunter_index(). */
constexpr int kLsw2HunterPageFirst = 6;
constexpr int kLsw2HunterPageLast = 7;
constexpr int kLsw2SelectPageSlots = 8;

/* Where the select screen keeps this hunter. False for a non-hunter, and
 * neither output is written then. */
bool lsw2_hunter_page_slot(int character_id, int* page, int* slot);

/* The character on a page and slot, or -1. `0x64` - the game's own "not
 * available" padding, which fills the tail of every short page - reads as -1
 * rather than as a character. */
int lsw2_select_character_at(int page, int slot);

/* Who the player was last. kLsw2DefaultHunter until something is remembered. */
int lsw2_last_hunter();

/* Remember a hunter. Ignores anything that is not one of the ten, and returns
 * whether the stored value changed. */
bool lsw2_set_last_hunter(int character_id);

/* Load a remembered value from the settings file. Same tolerance as the
 * setter: a file that has been edited to something impossible falls back to
 * Greedo rather than carrying an invalid id into the roster maths. */
void lsw2_last_hunter_load(int character_id);

/* Whether the value has changed since the last mark, so the settings file is
 * only rewritten when there is something to write. */
bool lsw2_last_hunter_dirty();
void lsw2_last_hunter_mark_clean();

/* WHERE THE GAME ITSELF TAKES A CHARACTER-CHANGE REQUEST.
 *
 * The level root carries one signed byte per player at
 * `+0x92E + player` and a countdown at `+0x932 + player`. A poller at
 * 0x02091604 runs every frame over all four, and when a byte is non-negative
 * and the countdown has expired it calls 0x020917C8 - the game's own "become
 * this character" - then clears the byte back to -1 and reloads the countdown
 * to ten frames.
 *
 * So the last-hunter apply is not a call the port has to make. It is a byte
 * the port writes and the game acts on, which is the same shape as the level
 * warp and the character-select page bounds, and the reason the two
 * constructor routes could be abandoned rather than forced. */
constexpr uint32_t kLsw2PendingCharacter = 0x92Eu;
constexpr uint32_t kLsw2PendingDelay = 0x932u;

/* The "nothing pending" value, from the MVN at 0x02091610 and the STRBs that
 * write it back at 0x02091668 and 0x02091674. */
constexpr int kLsw2PendingNone = -1;

/* Ask the game to make party slot 0 a given character.
 *
 * The general form of the apply below: the session's Roster rule needs the same lever
 * for a character that has nothing to do with the remembered hunter. Returns
 * false when there is nothing to do - guest unreachable, already that
 * character, or a request already in flight. */
bool lsw2_request_character(int character_id);

/* The same, for any party slot.
 *
 * The poller walks FOUR players - the pending byte is at
 * `+0x92E + player` and the countdown at `+0x932 + player`, one each. So a
 * machine can ask the game to change any station's character, not only its own,
 * and that is what makes every machine able to agree on who everybody is:
 * the character travels on every block's header, and this applies what arrived.
 *
 * A previous pass concluded the roster rules were local-only. They were, but
 * not because the lever was - because nothing had read the poller's stride. */
bool lsw2_request_character_for(int player, int character_id);

/* Ask the game to make party slot 0 the remembered hunter.
 *
 * Returns false when there is nothing to do - guest state unreachable, the
 * player is already that character, or a request is already in flight. Doing
 * nothing is the common case and is not a failure. */
bool lsw2_last_hunter_apply();

/* The lobby's party builder (0x02050C44's override) built `slot` while a
 * session is up - from the level's own data, so not this player's choice.
 * The tick then restores this console's own station through the game's
 * switch message, and records nothing for it until that is done - otherwise
 * joining a lobby would change the player's character to the level's Greedo
 * instead of the hunter they had before. */
void lsw2_last_hunter_note_build(int slot);

/* Called once a frame in the fork. Reads the player's character out of party
 * slot 0 while they are somewhere the answer means something - the arena
 * lobby, or a match - and remembers it. Also issues the apply once, on the
 * frame the player first arrives in the lobby.
 *
 * NOT EVERYWHERE, deliberately. The requirement is the "last-played Bounty
 * Hunter", and a hunter id observed while the player is walking around a story
 * level as a story character would be either stale or nonsense. Restricting to
 * the two places the fork puts the player is what makes the observation mean
 * last-played hunter. */
void lsw2_last_hunter_tick();

#endif /* LSW2_LASTHUNTER_H */
