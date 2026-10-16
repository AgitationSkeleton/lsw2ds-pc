/* Naming the game's states by watching it change them.
 *
 * WHY. The factory's jump table enumerates 58 states, 35 of them
 * implemented, but an arm gives a constructor, not a name. Naming them means
 * either reading 35 constructors - days - or watching which states the game
 * requests while someone walks from the Cantina to the arena, which is one
 * play-through. The network protocol was decoded with the same trade and it
 * was the right one.
 *
 * TWO BLOCKED FEATURES RUN THROUGH THIS. The pause menu's wireless row requests
 * a state it reads from `[menu + 0xF0]`, and the co-op screens ARE
 * states rather than entities - state 20 among them. Neither can
 * proceed while "which state is which" is unknown, and no `0x0205ECC4` call
 * site passes a literal, so it cannot be answered statically.
 *
 * READ-ONLY, NO OVERRIDE. Message type 0x13's handler shows where the current
 * state lives:
 *
 *     020B1AE8  LDR  r0, =0x02102900     the state manager
 *     020B1AEC  LDR  r0, [r0, #4]        its current object
 *     020B1AF4  LDRNE r0, [r0, #4]       the STATE ID, a word
 *     020B1AFC  CMP  r0, #0x22
 *
 * so the port reads the same two words.
 *
 * IT RUNS IN BOTH BUILDS, and that is the point rather than a convenience: the
 * two states worth naming are reached through the Cantina console and the
 * pause menu's wireless row, and the fork has removed both. See the comment on
 * the tick for why the obvious fork gate was the wrong one. `0x0205ECC4` has 176 callers and
 * overriding it would mean reimplementing the game's screen machinery
 * (`tools/sweep_override_targets.py` exists for exactly that reason). */

#ifndef LSW2_STATELOG_H
#define LSW2_STATELOG_H

#include <cstdint>

constexpr uint32_t kLsw2StateManager = 0x02102900u;
constexpr uint32_t kLsw2StateCurrent = 4u;   /* manager + 4 -> the object   */
constexpr uint32_t kLsw2StateId = 4u;        /* object  + 4 -> the state id */
constexpr int kLsw2StateCount = 0x3A;        /* CMP r9, #0x39 in the factory */

/* THE REQUEST RECORD AND THE BUILT OBJECT, in one place.
 *
 * These lived in cheats.cpp's anonymous namespace, and the moment a second file
 * needed them the `dup_constants` gate caught the copy - which is the gate
 * doing its job, and the reason they are here rather than duplicated with an
 * apology. Read off the state factory's level block at `0x0205F7F4`:
 *
 *     0205F818  STM  r5 {r1,r9}       ; +0x00 vtable, +0x04 the state id
 *     0205F81C  STR  r8,[r5,#0x8]     ; +0x08 the MODE
 *     0205F820  STR  r7,[r5,#0xC]     ; +0x0C the LEVEL
 *     0205F824  STR  r6,[r5,#0x10]    ; +0x10
 *
 * `kLsw2StateMode` is the field this project went a long time not knowing
 * it had. `FUN_0205ecc4`'s **r3** lands here, `FUN_0208c888` reads it seven
 * times per frame, and it decides whether entering a level SPAWNS the players
 * (0x0E, 0x12, 0x04 -> FUN_02089e60) or merely places them (0x0F) or runs the
 * walkaround (0x10). cheats.cpp's warp preserved whatever was already in it,
 * which is why re-entering the lobby never respawned anybody - see
 * lobbyrespawn.h. */
constexpr uint32_t kLsw2SlotResult = 0x00u;   /* manager + slot*4           */
constexpr uint32_t kLsw2SlotArgs = 0x48u;     /* manager + slot*0x20 + 0x48 */
constexpr uint32_t kLsw2SlotStride = 0x20u;
constexpr uint32_t kLsw2SlotStatus = 0x90u;   /* manager + 0x90 + slot      */
constexpr uint32_t kLsw2StatusRequested = 1u;
constexpr uint32_t kLsw2StatusBuilt = 3u;     /* the pump's "settled"       */

/* THE TWO SLOTS ARE TWO LAYERS, and the requester's third argument picks one.
 *
 * `0x0205ECC4(manager, state, slot, ...)` hands its r2 to 0x02001688, which
 * pushes the old [0x027E02A4] and stores r2 there, and 0x0205EC4C writes the
 * record at manager + [0x027E02A4]*0x20. So r2 IS the slot. The cartridge's
 * own round start, FUN_02072CF4 mode 3, uses both:
 *
 *     02072DC4  0x0205ECC4(m, 4,    0, 0x10, level 9)   slot 0: the WORLD
 *     02072DE0  0x0205ECC4(m, 0x29, 1, 0)               slot 1: the SCREEN
 *
 * and Quit Game does the same with 0x11 in slot 0 and 0x0F in slot 1.
 * lsw2_state_current() reads manager + 4, the screen slot's object - which is
 * why "state 54" and "level 9" are both true while Game Settings is open. */
constexpr uint32_t kLsw2WorldSlot = 0u;
constexpr uint32_t kLsw2ScreenSlot = 1u;

constexpr uint32_t kLsw2StateKind = 0x04u;
constexpr uint32_t kLsw2StateMode = 0x08u;    /* r3 of FUN_0205ecc4         */
constexpr uint32_t kLsw2StateLevel = 0x0Cu;
constexpr uint32_t kLsw2StateArgC = 0x10u;

/* The state class the factory's jump table builds for a level - case 4. */
constexpr uint32_t kLsw2LevelStateKind = 4u;

/* THE FRONT END'S WORLD STATE, 0x11: what Quit Game asks the world slot for
 * (measured as "request 17 from 0208EFD4", with 15 from
 * 0208EFF0 for the screen). Named because the world slot is NOT always a level
 * once the fork is up: the score screen's world is 43 (from 020B1AE0), so "not
 * a level" cannot mean "quit" - only this kind does (fork.cpp). */
constexpr uint32_t kLsw2FrontEndWorldKind = 0x11u;

/* THE LEVEL MODES THEMSELVES, as a set rather than as loose constants.
 *
 * An enum because these are five values of one thing, and reading them together
 * is the whole point: which of them a level is entered with decides whether the
 * players get SPAWNED. Every arm below was read off `FUN_0208c888`, which
 * branches on this field seven times per frame:
 *
 *     0208C9A8 / 0208CB6C   0x0E, 0x12 -> FUN_02089a6c + FUN_02089c88
 *                                          -> FUN_02089e60, the SPAWN dispatcher
 *     0208CB9C              0x04       -> FUN_02089e60 directly
 *     0208CB88              0x0F       -> FUN_020897f8, which writes player
 *                                          POSITIONS and calls nothing else
 *     0208CAF4              0x10       -> broadcast behaviour 6 and set the
 *                                          walkaround byte [r0+0x25C]
 *
 * `CoopEnter` is the one the cartridge's own `FUN_020b111c` passes when the
 * session tells every console to enter the level - which is why starting a
 * co-op game respawns you and re-entering the lobby with the mode already in
 * the state does not. See lobbyrespawn.h. */
enum class Lsw2LevelMode : uint32_t {
    Spawn = 0x04,        /* the spawn dispatcher, directly                 */
    Ordinary = 0x05,     /* NO arm in FUN_0208c888 - see below             */
    CoopEnter = 0x0E,    /* what FUN_020b111c passes; reaches the spawner  */
    Arena = 0x0F,        /* placement only - the networked deathmatch      */
    Walkaround = 0x10,   /* broadcasts behaviour 6, sets [r0+0x25C]        */
    SpawnAlt = 0x12,     /* the 0x0E arm's sibling, same dispatch          */
};

/* `Ordinary` (0x05) WAS MEASURED, NOT INFERRED, and it corrects this file's
 * first version. The lobby was assumed to run `Walkaround`; arming the lobby
 * respawn experiment printed what was actually there:
 *
 *     [lobbyrespawn] start_lobby: requested level 9 mode 0x0E
 *                    (was level 9 mode 0x05, ...)
 *
 * So the cantina/lobby state runs mode 0x05, and `FUN_0208c888` has **no arm
 * for it at all** - it is none of 0x04, 0x0E, 0x0F, 0x10, 0x12, so that update
 * does no per-player work on an ordinary level. cheats.cpp's own lobby warp
 * writes 5 for the same reason. The walkaround enable there comes from the zone
 * route instead, which is consistent with this and was not before. */

/* No `lsw2_mode_value()` helper. There was one, for a `write32` of the mode into
 * the request record; that whole approach was rejected and `sweep_orphans`
 * caught the helper the moment its last caller went. A cast at the one place
 * that needs a number is not worth a function nobody calls. */

/* THE TWO STATES THE PORT ACTUALLY TESTS AGAINST, named here because they were
 * named in four other places first.
 *
 * 0x29 was `kStateHunterSelect` in diagnostics.cpp, `kStateHunterSelect` again
 * in lasthunter_observe.cpp, and `kLsw2SelectStateKind` in selectskip.h. Three
 * copies under two names - and the two names are the SAME SCREEN seen from two
 * vocabularies: the base game's statue select is the fork's hunter select.
 * That is worth keeping in one place precisely because the two names made it
 * look like two facts.
 *
 * 0x2C was `kStateScore` in diagnostics.cpp and `kScoreState` in
 * scorescreen.cpp - two names one word apart, which is the hardest kind of
 * duplicate to see - plus the prose reference in the block above.
 *
 * 42 (0x2A), the arena match, is NOT declared here. The recordings name it and
 * the block above records it, but no code compares against it, and a constant
 * with no consumer is a layout claim nobody checks. It goes in when something
 * reads it. */
/* THE GAME'S STATE-BITS WORD, read by ~194 sites. Bits the port relies on:
 * 0x03 paused (always together - dispatch_overrides.cpp), 0x40 multi-view,
 * set in a wireless session - arena, and a lobby that has seated a player
 * - and 0x200 skips the frame loop's VBlank wait. */
constexpr uint32_t kLsw2GameStateBits = 0x027E02A8u;
constexpr int kLsw2StateSessionStart = 0x28;  /* 40, where message 0x0A mode 0 sends every console: the lobby (re)load */
constexpr int kLsw2StateSelect = 0x29;   /* the character/statue select */

/* This was first described as "the round counter message 3 carries", said to
 * change exactly once per round start. RETRACTED: it is a global
 * SEQUENCE counter - 0x0200FF2C..0x0200FF3C hand out `[it]++` as an actor's
 * +0x1B4 id, so it steps every time something is built - and message 3 only
 * resynchronises it (the host sends its value + 1). A per-round seed built on
 * it changed dozens of times a round (conn_music2). Message 3's copy is still
 * the round's first value, which is what diagnostics.cpp prints. */
constexpr uint32_t kLsw2SequenceCounter = 0x02101AD4u;
constexpr int kLsw2StateScore = 0x2C;    /* 44, the score screen        */
/* 53, the pause menu: the factory's kind 0x35 (0x0205FE00, constructor
 * 0x0208E260, vtable 0x020FF524) - what FUN_0208ce8c asks the screen slot for
 * (0x0205ECC4(m, 0x35, 1, 0, 0, 0)). */
constexpr int kLsw2StatePause = 0x35;
/* 38, the communication-error screen. The netgame tick asks the screen slot for
 * it when the link breaks (0x020B0BAC: 0x0205ECC4(m, 0x26, 1, 0, message 3)),
 * with world 15 beside it; a cross-player checksum mismatch raises it too. */
constexpr int kLsw2CommErrorScreen = 0x26;

/* 48 - THE CARTRIDGE'S OWN DEFAULT ARM, and what a clean arena lobby carries.
 *
 * Reading the factory shows state 48 has no real constructor of its
 * own: state 48 IS the default arm, in the cartridge's own factory, against
 * state 54 which does have one. So it is not a specific netgame lobby state -
 * it is what an ordinary level entry produces.
 *
 * That is exactly why it is the state to ask for when returning to the lobby.
 * Every clean lobby observed carries 48 over level 9; an observed crash was
 * state 42 - the MATCH state - over level 9, a pairing the game never makes
 * itself, and the per-frame update then walked a list the match had freed. */
constexpr int kLsw2StateDefaultArm = 0x30;

/* TWO STATES NAMED FROM A RECORDING, without the game running.
 *
 * out/work/archive/gpudump_3matches records three arena matches. Replaying its
 * RAM deltas and reading [[0x02102900 + 4] + 4] at every frame gives the state
 * timeline; the same capture's oam_sub says exactly when the arena HUD's slot
 * column exists, which brackets each match. The two line up three times:
 *
 *     match 1   state -> 42 at f015220   state -> 44 at f033150
 *     match 2   state -> 42 at f034740   state -> 44 at f052670
 *     match 3   state -> 42 at f053720   state -> 44 at f064470
 *
 * and the HUD column appears at f015220 / f034750 / f053720 and vanishes at
 * f033150 / f052670 / f064470 - the same frames.
 *
 *     state 42 (0x2A) is the arena MATCH
 *     state 44 (0x2C) is what follows one ending - the score screen
 *
 * AND 44 HAS BEEN SEEN, not only inferred from where it sits.
 * out/work/gpudump_select ends in state 44 and holds it for 12999 frames, and
 * its screen0.png/screen1.png are that state: the top screen reads "Scores"
 * with a Score / Kills / Deaths row per player beside the four coloured rings,
 * and the bottom screen is the between-rounds menu - Continue, Select
 * Character, Settings, Quit. A capture idling there was the one thing that
 * could have contradicted the bracketing above, so it was checked rather than
 * assumed; it agrees.
 *
 * 44 CONFIRMS A NUMBER THE PORT ALREADY USED. scorescreen.cpp has
 * `kScoreState = 0x2C`, which is 44, and it had been read out of the code
 * rather than watched. Three match endings in a row is now behind it.
 *
 * Between rounds the sequence is 44 -> 54 -> 41 -> 42, consistently all three
 * times. Those two are NOT named here: appearing between a score screen and
 * the next match is a position, not a meaning.
 *
 * WHAT THE RECORDINGS CANNOT NAME, checked so nobody searches them again.
 * Every gpudump capture in out/work is the arena flow. Replaying 38 of them
 * and collecting every state id reached gives exactly nine, plus -1:
 *
 *     15, 30, 32, 33, 41, 42, 44, 48, 54
 *
 * (28 also appears, in gpudump_3matches' early frames.) There is no state 20 -
 * the co-op screen - and nothing that could be the Cantina console or
 * the base game's pause wireless row. So the master server's Cantina-console
 * and pause-menu entry points, which depend on those two states, cannot be
 * settled from what has been recorded: naming those states genuinely needs
 * the game running, and that is what a live run is FOR.
 *
 * ONE FRAME READ AS 539506744, which is not a state at all. The manager's
 * object pointer was mid-update on that frame, so the chase landed on
 * something else. It is left in this note because it is the honest shape of
 * this measurement: a pointer chase sampled once per frame will occasionally
 * catch a tear, and a single-frame reading of any of these is worth nothing on
 * its own. The three matches agreeing is what makes the mapping above safe. */

/* The state the game is in, or -1 when the manager has no current object.
 * Values outside 0..0x39 are returned as read - a state id the factory would
 * reject is worth seeing rather than hiding. */
int lsw2_state_current();

/* THE STATE THE SCREEN SLOT HAS BEEN ASKED FOR AND NOT YET BUILT, or -1.
 *
 * The pump at 0x0205F45C builds a request and then zeroes its id word
 * (0205F4A8 `str r1(=0), [r0, #0x48]`), so a non-zero id there is a request
 * the machine has accepted and not acted on yet. That is the one reading that
 * knows where a transition is GOING while the old screen is still current and
 * the world slot is already unloading - the window in which "no level" looks
 * the same for a quit and for the round start. */
int lsw2_state_pending();

/* THE CURRENT STATE OBJECT ITSELF, or 0 when there is none.
 *
 * ONE COPY OF THE READ, BECAUSE FOUR MODULES HAD TWO DIFFERENT ONES AND ONE OF
 * THEM WAS WRONG.
 *
 * `kLsw2StateManager` is the manager's BASE ADDRESS, so the current-state
 * pointer is `read32(kLsw2StateManager + kLsw2StateCurrent)` - one read.
 * scorescreen.cpp, coopjoin.cpp, lasthunter_observe.cpp and diagnostics.cpp all
 * did `read32(kLsw2StateManager)` FIRST and then added the offset, which is one
 * dereference too many and reads whatever happens to be at the manager's first
 * word.
 *
 * That is not a style difference. It is why the fifteen-second score
 * screen timer never fired in a real match: the id never matched 0x2C, so the timer
 * was never armed, and the screen sat for sixty seconds while the module that
 * exists to dismiss it saw nothing at all.
 *
 * Anything that needs the state object should call this rather than open-code
 * the chain, so there is one place to be right. */
uint32_t lsw2_state_current_object();

/* Once a frame from the fork tick. Reports the state when it changes, with a
 * running trail, and nothing otherwise. */
void lsw2_statelog_tick();

/* WALL-CLOCK, SO TWO CONSOLES' LOGS CAN BE PUT IN ORDER.
 *
 * The goal this serves, in plain english: the match-start failure is now down
 * to one question - does the child lose its wireless connection BEFORE or AFTER
 * the host tears its own data sharing down? Both happen inside the same
 * five-second sample, and the two consoles write separate log files with no
 * common reference, so line numbers cannot answer it. The two
 * processes run on one machine and therefore share a wall clock, which is the
 * only ordering they have in common.
 *
 * Returns "HH:MM:SS.mmm" in a static buffer - single-threaded logging only, and
 * the caller must print it before calling again. Host-side entirely: it reads
 * the host's clock and touches nothing in the guest. */
const char* lsw2_log_clock();

#endif /* LSW2_STATELOG_H */
