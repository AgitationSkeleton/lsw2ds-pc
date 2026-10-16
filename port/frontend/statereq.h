/* Who asks for which screen - a question that was parked and is now partly
 * answered and partly overtaken.
 *
 * === WHAT ONE DRIVEN RUN SHOWED =================================
 *
 * THIS OVERRIDE PRINTED NOTHING. Driven over the debug surface through boot,
 * the title, a save load, the Cantina, the pause menu and back, `statelog`
 * reported six state transitions - including state 53, the pause menu, which
 * no recorded capture had ever reached - and this file reported none of them.
 *
 * It is installed: `kOverridesAlways` is registered before
 * lsw2_register_dispatch_overrides' fork-only return, in both builds, and
 * tools/sweep_override_reachable.py finds no direct call sites for any of the
 * ten registered addresses, this one included. So either 0x0205ECC4 is not
 * what changes the state - the premise below - or it is and the override is
 * not being reached in a way that sweep cannot see. The run does not say which
 * and this note does not guess.
 *
 * AND THE QUESTIONS IT WAS PARKED FOR HAVE MOVED ON ANYWAY. The game half of
 * the baseline's co-op hosting was verified end to end in the same run: the
 * pause menu's wireless row starts the fork's lobby, the fork writes
 * [owner + 0x06], and the cartridge's own builder renames the row to "Shut Off
 * Wireless" one screen later. The co-op join's question of "which state does
 * the row request" is moot because coopjoin.cpp replaces
 * 0x020B10A8 and the request never happens.
 *
 * The paragraphs below are the reasoning that built this probe. They are kept
 * because the probe is still the right instrument for the Cantina console,
 * which the run did not reach - but read them knowing it has never produced a
 * line.
 * ================================================================
 *
 * WHAT IS BLOCKED. The search for the Cantina console was reframed: the
 * co-op screens are not entities with handlers, they are STATES, and a state is
 * entered by requesting it through `0x0205ECC4`. So "find the console" became
 * "find who requests the co-op state", and it was established that no call
 * site passes a literal - the state arrives from a record - so it cannot be
 * answered by reading. The same wall stands on the pause menu's
 * side: row 1 requests `[menu + 0xF0]`, and nothing in the whole
 * `0x0208Dxxx-0x0208Exxx` range ever writes that field.
 *
 * WHY THIS AND NOT statelog.h. `statelog` reports the state AFTER it changes,
 * by polling the manager's current object. That names states, which is half the
 * question. It cannot name the REQUESTER, and "which code opened this screen"
 * is the half the baseline's co-op host and join actually need.
 *
 * This sits on the request itself, so it sees both: the state in `r1`, and the
 * caller in `lr`. One walk through the Cantina and the pause menu produces the
 * mapping that three earlier investigations each stopped short of.
 *
 * WHY IT IS SAFE TO SIT ON A FUNCTION WITH 176 CALLERS. Because it does not
 * replace it. `0x0205ECC4` has **zero direct call sites** in the generated code
 * - every one of those 176 callers reaches it through `runtime_dispatch` - so an
 * override is reached, and it delegates by calling the lifted symbol, which does
 * not re-enter the dispatch table and so cannot recurse. Nothing about the
 * game's behaviour changes.
 *
 * That reachability is checked rather than assumed. Earlier, the plan to
 * override `0x0208EA58` was abandoned for the opposite finding: all four of its
 * callers are in the same generated bank, so the recompiler calls it by symbol
 * and an override there can never fire. `tools/sweep_override_reachable.py`
 * measures the difference.
 *
 * BOTH BUILDS. The Cantina and the pause menu's wireless row exist in the
 * baseline; the fork warps past one and hides the other. Gating this to the
 * fork would repeat statelog's original mistake exactly. */

#ifndef LSW2_STATEREQ_H
#define LSW2_STATEREQ_H

#include <cstdint>

/* `0x0205ECC4(manager, state, flags)` - the game's own "change screen". */
constexpr uint32_t kLsw2StateRequester = 0x0205ECC4u;

/* RETRACTED: "WHICH SLOT A REQUEST LANDS IN, and it is the GAME's
 * choice". This named 0x027E02A4 - which 0x0205EC4C does read - as the game
 * choosing a slot, measured at 1 "while a lobby was up". It is the manager's
 * CURRENT slot: 0x0205ECC4 sets it from its own r2 (via 0x02001688) before
 * writing, and the frame loop walks it 0, 1 and restores it, so between frames
 * it is usually 1 and sometimes 0. A request written into "its" slot therefore
 * sometimes landed in the world slot - round two's Game Settings did exactly
 * that - and the constant went with the fix: the port names the slot
 * (statelog.h, kLsw2WorldSlot / kLsw2ScreenSlot) as every caller of 0x0205ECC4
 * does. The offset it pointed at is statelog.h's kLsw2SlotArgs.
 *
 * A REQUEST IS EIGHT WORDS, not one. 0x0205EC4C writes, at manager + slot*0x20:
 *   +0x48 the state id, +0x4C the MODE (the requester's r3 - once read here as
 *   "the flag the callers pass as 1"), +0x50 the level, +0x54 an argument, and
 *   +0x58 through +0x64 zeroed. Writing only the first would leave stale words
 *   from whatever the last request was. */
constexpr uint32_t kLsw2StateRequestWords = 8u;

/* The override. Reports each distinct (state, caller) pair once, then runs the
 * cartridge's own code unchanged. */
void lsw2_statereq_override();

/* A REQUEST INTO A NAMED SLOT, WRITTEN EXACTLY AS 0x0205EC4C WRITES IT, and
 * nothing else.
 *
 * `0x0205ECC4(manager, state, slot, mode, level, arg)` stores, at
 * manager + slot*0x20 + 0x48: state, mode, level, arg and four zero words - and
 * does NOT touch the slot's status byte. The manager's own per-slot transition
 * (0x0205EF84) notices the request, fades the slot out, calls the old state's
 * exit (vtable+0x10) and destructor (vtable+4), and only then sets status 1 for
 * the pump to build the new one.
 *
 * The fork's cold warp set status 1 itself, which builds the new state over
 * the old one without ever exiting or destroying it. From the boot splash that
 * leak was harmless; after a four-player match it left the old world's
 * registrations live under the new lobby, and all four consoles' sprite loaders
 * then read sources that are not data. This is the teardown-respecting way in -
 * and now the only one: cheats.cpp's warm and cold warps and its
 * game-UI install write their requests through this too, so the boot, the
 * post-Disconnect return and the door's backstop tear down what they replace.
 *
 * `slot` is the world (0) or the screen (kLsw2ScreenSlot, 1); anything else
 * writes nothing and returns false. */
bool lsw2_state_request_in_slot(uint32_t slot, uint32_t state, uint32_t mode,
                                uint32_t level, uint32_t arg);

#endif /* LSW2_STATEREQ_H */
