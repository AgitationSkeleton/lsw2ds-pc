/* The host settings menu's Roster rule in a networked session.
 *
 *   Roster: Hunters / ALL / Random Hunter / Random ALL / One. Random Hunter
 *   gives each player a random hunter every time they spawn; Random All gives
 *   any random character every time they spawn, except the three custom
 *   character slots. Any setting besides Hunters skips the character select
 *   phase of the arena flow.
 *
 * Offline, rosterenforce.cpp and selectskip.cpp do this from the fork's tick.
 * In a session that is forbidden (vwh.h, lsw2_vwh_lockstep): a character that
 * changes on one console a frame before another is the divergence the match's
 * own check ends in state 38. So in a session every change happens at a point
 * all consoles reach at the same step - which the game's own round supplies:
 *
 *   WHERE THE CHARACTERS COME FROM (a map checked here against the
 *   disassembly). The arena reads each player's character from the per-station
 *   save record - u16 at 0x02106FD8 + i*0x40, with a gate byte at +0x1B - once,
 *   when it builds (0x020897F8, called through the dispatch table from
 *   0x0208CB94). Message 3, which ends the statue select, fills those records
 *   from the statues picked (hunters only). A respawn does not read them: it
 *   rebuilds the dead actor's own character through 0x02091B88 (the call at
 *   0x020381C8 dispatches, returning to 0x020381CC).
 *
 * So:
 *
 *   1. THE HOST DECIDES, once a round, when the statue select opens: each
 *      present station's character - its lobby character (ALL), the host's
 *      pick (One), or a roll (the Random rules) - and posts it as an effect,
 *      numbered, for every rule (Hunters' carries only the number, which the
 *      random music draws from).
 *   2. EVERY CONSOLE APPLIES IT at the dataset that carries it: the round's
 *      table is stored, and - every rule but Hunters skips the select - each
 *      present player is marked as having chosen, so the host's own select
 *      update sends message 3 and the round starts.
 *   3. THE ARENA'S BUILD writes the table into the records before it reads
 *      them (an override on 0x020897F8), so a non-hunter - Gonk Droid under
 *      One - arrives in the arena as itself.
 *   4. A RESPAWN under a Random rule rolls again (an override on 0x02091B88 at
 *      the respawn's call): the roll is a pure function of the station, that
 *      station's spawn count and the round's salt, all of which every console
 *      holds identically, so no console has to tell another what it rolled.
 *
 * Hunters is the game's own select and is left alone. */
#ifndef LSW2_SESSIONROSTER_H
#define LSW2_SESSIONROSTER_H

#include <cstdint>

/* Once a frame from the fork tick: the host's decision (1), and forgetting the
 * round's table on every console once it is back in the lobby. */
void lsw2_sessionroster_tick();

/* The effect's payload, at the dataset boundary on every console (2). */
void lsw2_sessionroster_apply(const uint8_t* payload, int len);

/* The round the host numbered in the effect this console last applied, or 0
 * outside a session's round - the same on every console from that dataset on.
 * musicpick.cpp seeds the round's track from it. */
uint32_t lsw2_sessionroster_round();

/* The two overrides (3, 4). Fork-only (kOverridesCore). */
void lsw2_sessionroster_arena_build();
void lsw2_sessionroster_spawn();

/* TEST ONLY (vwh.h kLsw2EffectTestKill, posted by the host when LSW2_TEST_KILL
 * is set): at the dataset that carries it, on every console, the station's
 * actor dies the game's own way - its vtable's +0x40, which FUN_02004f7c calls
 * when a hit takes the last heart - pinned to the HUD's frame step so it runs
 * at the same point of the same frame everywhere. The respawn that follows is
 * where a Random rule re-rolls (4). */
void lsw2_sessionroster_test_kill(int station);

/* TEST ONLY (vwh.h kLsw2EffectTestTeleport, posted by the host when
 * LSW2_TEST_TELEPORT="station,x,y,z,seconds" is set; x y z in world units):
 * at the dataset that carries it, on every console, the station's actor is
 * placed at that position. A raw write of the position the actor carries at
 * +0xB0/+0xB4/+0xB8, made at the dataset boundary, so every console makes it
 * at the same step. */
void lsw2_sessionroster_test_teleport(const uint8_t* payload, int len);

/* TEST ONLY (vwh.h kLsw2EffectTestHit, posted by the host when
 * LSW2_TEST_HIT="station,attacker,type,seconds" is set): at the dataset that
 * carries it, on every console, the station's actor takes one hit from the
 * attacker's through the game's own hit handler, FUN_02004f7c(actor, attacker,
 * 0, type) - the vtable slot a shot or a swing reaches - pinned to the HUD's
 * frame step like the test death. `type` indexes the handler's damage table
 * at 0x020F0214 (type 1: two hearts, no special flags). The hat-stays
 * fix (hatstay.h) needed a hit a drive could not aim. */
void lsw2_sessionroster_test_hit(const uint8_t* payload, int len);

#endif /* LSW2_SESSIONROSTER_H */
