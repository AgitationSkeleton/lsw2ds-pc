/* Guest-state cheats: the only part of the shell that reaches INTO the game.
 *
 * THIS IS NOT PRESENTATION AND IT IS NOT PRETENDING TO BE. port/ARCHITECTURE.md
 * draws the line at "the host layer may change what the player experiences and
 * must not change what the game computes". Everything here crosses that line on
 * purpose, so it is kept in one file, gated on the cartridge SHA-1 by the same
 * bank registration the rest of the port uses, and each address is written down
 * with how it was found rather than asserted.
 *
 * WHAT IS KNOWN, and the distinction matters because half of it is not
 * actionable yet:
 *
 *   THE LEVEL INDEX is located and verified, by two independent paths that
 *   agree. It can be read and written.
 *
 *   WHAT MAKES THE GAME ACT on a changed index is NOT known. Writing the index
 *   tells the game where it thinks it is; it does not start a load. So "warp"
 *   here is honestly labelled as what it actually does, and the UI says so.
 *
 *   THE UNLOCK FLAGS are not located at all. There are no unlock
 *   buttons rather than buttons that quietly do nothing.
 *
 *   ^ SUPERSEDED. They were located; see "THE UNLOCK FLAGS." below,
 *   which explains the correction and why the sentence above is kept rather
 *   than edited away. The pointer is here because this paragraph is the first
 *   thing anyone reads, and a superseded claim with its correction two hundred
 *   lines down is a claim that gets believed.
 */

#ifndef LSW2_CHEATS_H
#define LSW2_CHEATS_H

#include <cstdint>

/* Whether guest state can be reached at all right now. False before the game
 * has built its context - during boot, and on any frame where the pointer chain
 * does not resolve - so every caller has one thing to check rather than each
 * inventing its own guard. */
bool lsw2_cheats_available();

/* The level the game currently believes it is in, or -1 when unavailable.
 * Read through the context pointer; see cheats.cpp for the chain. */
int lsw2_cheats_current_level();

/* THE LEVEL-STATE OBJECT'S OWN IDEA of where it is, which is NOT the same fact
 * read a second way - and calling it that was wrong.
 *
 * lsw2_cheats_current_level reads the REQUEST: the context the state machine
 * builds a level from. This reads the INSTALLED state object at
 * kLsw2LevelRootAddr + 0x68, which the game's ordinary entry path
 * (0x02090F3C) writes. The pump-driven warp does not write it - it installs
 * the new state in the manager's own slot table instead.
 *
 * So after a warp the two legitimately differ, and treating that as evidence
 * of a broken address was a false alarm that fired during normal use. They are
 * both worth showing - together they say whether the game arrived somewhere the
 * ordinary way or was put there - but a difference is information, not a
 * fault. */
int lsw2_cheats_current_level_via_state();

/* Write the level index the game reads. Returns false if guest state is not
 * reachable.
 *
 * DOES NOT LOAD THE LEVEL - it only changes where the game believes it is.
 * lsw2_cheats_warp is the one that actually goes there. */
bool lsw2_cheats_set_level(int index);

/* ACTUALLY LOAD A LEVEL, using the game's own state-request queue.
 *
 * Nothing is called and no registers are touched. The game has a request slot
 * with an argument block and a status byte; its own pump checks that byte every
 * frame, and when it is 1 the pump builds the requested state and calls the
 * state's enter method - which is the code that loads a level.
 *
 * RETRACTED IN PART: an earlier version read "So this fills in the arguments and
 * sets the byte". Setting the byte is the pump's half only. The manager's own
 * transition (0x0205EF84) is what sets it, after fading the slot and calling
 * the old state's exit and destructor; setting it from here built every warp
 * over a live old state that was never torn down - one of the causes of freed
 * sprite sources being read after a match. This now writes the request exactly as
 * 0x0205EC4C does (lsw2_state_request_in_slot) and leaves the byte to the
 * game, so the transition happens on the game's thread, at a moment the game
 * chose, with the game's own bookkeeping - teardown included.
 *
 * That is why this is a warp and lsw2_cheats_set_level is not.
 *
 * Returns false when guest state is unreachable or the current state is not a
 * level state - warping out of a menu is not something this knows how to do. */
bool lsw2_cheats_warp(int index);

/* Whether lsw2_cheats_warp would currently be able to act, so the UI can
 * disable the button rather than let it fail silently. False while a request
 * is still waiting in the world slot's block - a warp takes the transition's
 * fade and teardown now, several frames, and must not be posted twice. */
bool lsw2_cheats_can_warp();

/* A COLD warp: request a level without an existing level state to copy from.
 * See the long note in cheats.cpp - this supplies the two arguments that
 * lsw2_cheats_warp copies, using the pair the game itself used in the Cantina,
 * and is what lets the fork start in the Arena Lobby instantly rather than
 * fast-forwarding the boot. */
bool lsw2_cheats_warp_cold(int index);

/* THE COLD WARP WITH THE MODE SPELLED OUT, because the mode is not a detail -
 * it decides whether anybody is spawned.
 *
 * `lsw2_cheats_warp_cold` writes mode 5, and statelog.h names 5 `Ordinary`,
 * recording that it has **no arm in FUN_0208c888**. The cartridge's own co-op
 * entry asks for the level with mode **0x0E**, `CoopEnter` - "reaches the
 * spawner". Read out of the decompilation of the co-op start:
 *
 *     FUN_02088b84(container, level);
 *     FUN_0205ecc4(..., 4, 0, 0xe, level, 0);   <- kind 4, mode 0x0E
 *
 * That one argument is the whole difference behind the observed defect: starting
 * the lobby did not respawn the player there the way starting a co-op game does.
 * The port had been asking for the level in a mode that does not reach the
 * spawner.
 *
 * The no-argument form keeps mode 5 so nothing that already works changes. */
bool lsw2_cheats_warp_cold_as(int index, uint32_t mode);

/* The cold warp's only precondition: the state manager exists and is idle -
 * the world slot built, with no request waiting in its block. */
bool lsw2_cheats_manager_idle();

/* The state class in manager slot 0 or 1, or -1 when empty.
 *
 * MEASURED KINDS, read out of a running boot rather than guessed:
 *     6   the boot splash            (slot 0, from power-on)
 *    17   a front-end menu           (slot 0, after the splash)
 *     4   a level                    (slot 0, in game)
 *    15   a long-lived UI state      (slot 1, throughout - not the splash) */
int lsw2_cheats_state_kind(int slot);

/* Replace slot 1's boot UI (kind 15) with the in-game one (kind 48). Needed
 * after a cold warp, which leaves slot 1 exactly as the boot left it - and that
 * is why the lobby's touch screen showed the splash. Returns false when it is
 * already the game UI or the manager is busy. */
bool lsw2_cheats_install_game_ui();

/* ------------------------------------------------------------------------ */
/* THE UNLOCK FLAGS.
 *
 * The header above says these are "not located at all". That was true for a
 * long time and it is no longer; the correction is left visible rather than
 * edited away, so the claim's history stays readable.
 *
 * What the ROM says, read out of the accessors themselves rather than inferred:
 *
 *   0x02086534(block, n)   test  *(u32*)(block + 0x44CC + (n>>5)*4) bit n&31
 *   0x02086458(block, n)   set   the same array, then consults the second
 *   0x02086588(block, n)   test  *(u32*)(block + 0x44DC + (n>>5)*4) bit n&31
 *   0x02086558(block, n)   set   that second array
 *
 * and the dispatcher passes block = 0x02104D90 with n the row index of a
 * 108-entry cheat-code table at 0x020FED9C. The four codes of interest are rows 0-3.
 *
 * TWO ARRAYS, 0x10 BYTES APART. That gap is what fixes the first array's size:
 * four words, 128 bits, which is exactly enough for 108 codes and not enough
 * for a fifth word to exist before the second array starts. The second array's
 * extent is NOT established - nothing bounds it - so four words is what is read
 * and that is a choice, not a measurement.
 *
 * WHAT EACH ARRAY MEANS IS NOT ESTABLISHED EITHER. The dispatcher tests the
 * first and, depending on a byte in the roster table, sets one or the other.
 * Rather than guess at the semantics, the port reads both, and the fork's
 * unlock-everything is built from a captured pair taken from a save that is
 * known to have all four codes entered - measured, not reasoned. */
constexpr int kLsw2FlagWords = 4;

/* ALL TEN FLAG ARRAYS, because the refutation above only tested two of them.
 *
 * The readout used to cover 0x44CC and 0x44DC alone, and a capture showed those
 * two read identically on a fully-unlocked save and a fresh one - which is why
 * there are still no unlock buttons.
 *
 * A later pass then enumerated the accessors rather than guessing, by scanning for
 * the one idiom every flag array is reached through, and found NINE arrays (it
 * first counted ten; a recount corrected it). So
 * the earlier capture refuted two candidates out of ten and the conclusion
 * drawn from it - "the flags are not located" - was broader than the evidence.
 *
 * This makes the next capture decisive instead of partial: read all ten on both
 * saves and the array that differs is the one. */
struct Lsw2FlagArray {
    uint32_t offset;      /* from the flag block */
    const char* note;     /* what is known about it, honestly */
};
constexpr int kLsw2FlagArrayCount = 9;
extern const Lsw2FlagArray kLsw2FlagArrays[kLsw2FlagArrayCount];

/* Reads kLsw2FlagWords words from each of the ten. `out` must have room for
 * kLsw2FlagArrayCount * kLsw2FlagWords. */
bool lsw2_cheats_read_all_flag_arrays(uint32_t* out);

/* EVERYTHING UNLOCKED FROM THE START. Sets OWNED and SEEN for every character
 * and vehicle id the game ships, and marks all 22 terminal Extras as bought.
 *
 * Both roster arrays, deliberately: the Characters tab tests SEEN first and
 * draws "????" when it is clear, so OWNED alone gives a roster of question
 * marks.
 *
 * AND THE CUSTOMIZATION PARTS - "all character customization parts and weapons"
 * - which this header used to say it did NOT do, on the grounds that the array
 * holding them was unidentified. It was identified and the function was
 * extended; this sentence was not, and a contract that describes a function as
 * refusing to do something it does is worse than no contract, because it reads
 * as authoritative. It cost a re-derivation before it was noticed.
 *
 * Does NOT enable Extras - that is +0x4518 and belongs to the host's match
 * settings, not to unlocking. Setting the enable array would take that decision
 * away from the host, which the pause menu and the host settings give them. */
bool lsw2_cheats_unlock_everything();

/* THE HOST SETTINGS' EXTRAS: switch the host's chosen Extras on, and everything
 * else off.
 *
 * `enabled` is indexed the way hostsettings.h's table is - which is the same
 * index the game uses for both `+0x4514` and `+0x4518`, established from the
 * Extras terminal's own toggle handler. The game's clear-all loop
 * runs eighteen entries, so that is the ceiling.
 *
 * Writes every bit rather than only the set ones: turning an Extra off has to
 * turn it off. */
constexpr int kLsw2ExtrasEnabledCount = 18;
bool lsw2_cheats_set_extras(const bool* enabled, int count);

/* APPLY ONE EXTRA THROUGH THE CARTRIDGE'S OWN TOGGLE (host settings, opt-in).
 *
 * lsw2_cheats_set_extras writes the +0x4518 bit directly, which the per-frame
 * readers see but which SKIPS the side effects the cartridge's own toggle
 * FUN_02086810 runs: theme music for Disco/Old Time/Old West/Medieval, the
 * Letterbox enable/disable, the party re-dress, and the mutual-exclusion of the
 * ten theme flags. So a host's Disco floor appears but plays no music, and two
 * themes can be "stacked" the game would never hold at once.
 *
 * This queues the cartridge's toggle - through the gear hop, because it starts
 * music and re-dresses the party and so needs the guest's live scheduler - for
 * bit `i` if and only if its current state differs from `wanted`. The toggle
 * flips, so calling it exactly on a difference sets the bit right and lets the
 * game do its own side effects and exclusion. Returns true while a change is
 * still pending for this bit (it differed and a toggle was queued or refused),
 * false when the bit already matches. */
bool lsw2_cheats_toggle_extra_if_needed(int i, bool wanted);

/* Extra `i`'s enabled bit (+0x4518) as the game holds it now: 1 or 0, or -1
 * before the game's flag block exists. */
int lsw2_cheats_extra_enabled(int i);

/* CUSTOMIZATION AUTO-APPLY, through the game's own trigger.
 *
 * Switching to another character did not carry the customizations onto it
 * automatically - observed twice, against two different attempts at writing the customize slots and
 * hoping the game re-read them.
 *
 * It does not re-read them. What it DOES do, every frame, is walk the party in
 * `0x020897F8` and for each slot read the appearance record at
 * `0x02106FD8 + slot*0x40`:
 *
 *     if (character != 0x64 && gate != 0)  ->  0x02091B88 applies the record
 *
 * where `gate` is the byte at record + 0x1B. The game's own writers confirm the
 * polarity: `0x020895D8` clears a slot with character 0x64 AND gate 0, and
 * `0x02089440` populates a record and sets the gate.
 *
 * So the appearance is not something to be smuggled in before a spawn - it is
 * REQUESTABLE, and this asks. Which is the project's standing preference and
 * the reason two cleverer attempts failed: fill in the game's own data and let
 * the game's own code act on it. */
constexpr uint32_t kLsw2AppearanceRecord = 0x02106FD8u;
constexpr uint32_t kLsw2AppearanceStride = 0x40u;
constexpr uint32_t kLsw2AppearanceGate = 0x1Bu;

/* THE RECORD'S HEARTS, the byte below the gate. `0x02089440` (FUN_02089388)
 * stores the low byte of actor+0x6B6 here when the game snapshots the party
 * before leaving a level, and FUN_0208a31c writes it back into the new actor
 * when a gated record is restored. The record array's constructor
 * (FUN_0208896c) fills the whole object with 0xFF through FUN_0205e624 and
 * never sets this byte, so a record gated WITHOUT a snapshot restores 255
 * hearts - see scorescreen.cpp's refill_party_hearts. The game's own offsets
 * are +0x21FE and +0x21FF from the record array's base: the hearts byte is the
 * gate's neighbour, and is written that way rather than as a second literal. */
constexpr uint32_t kLsw2AppearanceHearts = kLsw2AppearanceGate - 1u;

/* THE COUNT EVERY PLAYER IS BUILT WITH: FUN_0200d6f4, the player actor's
 * constructor, sets both its hearts (+0x6B6) and its most (+0x6B8) to 4, and
 * FUN_02089ba8's party reset writes the same 4. What the fork writes into a
 * record whose hearts byte a reload is about to restore - the lobby return
 * (scorescreen.cpp) and the ALL pick's lobby reload (freeplaypick.cpp). */
constexpr uint8_t kLsw2PlayerBuiltHearts = 4;
/* The empty-field value is kLsw2CharacterNone above. */

bool lsw2_cheats_request_appearance(int slot);

/* THE SOLO LOBBY'S DISABLED EXIT DOOR, met by refusing the transition rather
 * than by finding the entity.
 *
 * Call once a frame. If a level change is pending for anywhere other than
 * `level`, its requested index is rewritten so the game reloads where it is.
 * Returns true on the frames it actually redirected something.
 *
 * IT NO LONGER RACES THE PUMP. This line used to end "that race is unmeasured -
 * if it loses, the player leaves and the fallback is to warp back", and the
 * definition in cheats.cpp was corrected: a live run measured the
 * race, the request turned out to be posted and consumed inside one frame, and
 * the fix was to test the request's ARGUMENTS rather than its status - which
 * removed the window instead of racing it. The warp-back fallback was never
 * needed.
 *
 * The declaration kept the old wording for a day because only the definition
 * was edited. Two copies of one claim, and the stale one is the one a reader
 * meets first. */
/* THE TWO ARENA MAPS, named once, and NOT in levels.h - that file says
 * "Generated ... do not edit" at the top and a regeneration would silently take
 * this with it. A hand-written rule belongs in a hand-written header.
 *
 * Ids 4 and 5 - Jabba's Tower and Mos Eisley Streets - are the only two the
 * generated table marks "confirmed"; the later "Arena (n)" rows are derived from
 * asset names and have never been reached, so they are deliberately excluded. A
 * guess here would make the lobby door refuse a real transition or admit a wrong one.
 *
 * The pin below is the consumer: it must refuse the way OUT of the lobby without
 * refusing the way INTO a match. */
inline bool lsw2_level_is_arena(int level) {
    return level == 4 || level == 5;
}

bool lsw2_cheats_pin_level(int level);

/* ------------------------------------------------------------------------ */
/* THE PARTY, for the solo lobby's party of one.
 *
 * `config/free_play_pages.json` records the array: four slots at
 * 0x0210930C + 0x4C + i*4, each a pointer to an actor, with a member counted as
 * present when `actor+0x08 & 0x803 == 1` and its character id at actor+0x6BB.
 *
 * REMOVING ONE TAKES TWO WRITES, and which two was settled by trying them on a
 * live game rather than by reading more code:
 *
 *   clear bit 0 of actor+0x08   the companion vanishes from the WORLD
 *   zero the slot pointer       the Party Members tab drops to one portrait
 *
 * Either alone is visibly wrong - the first leaves two portraits on the touch
 * screen, the second leaves a second Greedo standing in the room. Both were
 * captured before and after.
 *
 * THIS BEHAVIOUR IS THE PORT'S OWN, NOT THE ROM'S, flagged here as
 * port/ARCHITECTURE.md requires. The game has no "party of one" mode; this
 * makes one by clearing the state the game's own presence test reads. */
constexpr uint32_t kLsw2PartySlots = 4;

/* THE ARRAY ITSELF, and the actor byte it leads to. Both were written three
 * times over before this file was given them:
 *
 *   0x4C    cheats.cpp's kPartyBase and partyportrait.h's kLsw2PlayerSlotActor
 *   0x6BB   cheats.cpp's kActorCharId, partyportrait.h's kLsw2ActorCharacter
 *           AND headfix.cpp's kActorCharacter - three copies, in three files
 *   4       this file's kLsw2PartySlots and partyportrait.h's
 *           kLsw2PlayerSlotCount
 *
 * The base is an OFFSET FROM THE LEVEL ROOT, which levels.h generates as
 * kLsw2LevelRootAddr: partyportrait.h also held a fourth copy of the root
 * address itself, under the name kLsw2PlayerSlots, which is why that file's
 * arithmetic looked self-contained and did not read as a duplicate of the four
 * .cpp files that already went through levels.h.
 *
 * kLsw2PlayerSlotActor was the worst of the names - it reads as "the actor
 * field inside a player slot" when it is the offset at which the whole array
 * begins. The address it built was right; the name described a structure that
 * does not exist. */
constexpr uint32_t kLsw2PartyActors = 0x4Cu;
constexpr uint32_t kLsw2ActorCharacter = 0x6BBu;
/* The actor's heading, u16, 0x10000 a full turn: what the arena camera frames
 * behind (enhanced.cpp) and what the radar turns by (radarview.h). */
constexpr uint32_t kLsw2ActorHeading = 0xCAu;
/* The actor's view index, u8: the station its deaths are scored to
 * (FUN_02004dec), and 4 for "no view of its own" (cheats.cpp, killcredit.h). */
constexpr uint32_t kLsw2ActorViewIndex = 0x5DAu;

/* THE ACTION ROW, eight bytes along from the character id and part of the same
 * actor. gearapply.cpp had it as kActionRow and partslot.cpp as
 * kActorActionRow, both citing the same finding - the same actor, two
 * names. */
constexpr uint32_t kLsw2ActorActionRow = 0x6C3u;

/* THE "NO CHARACTER" SENTINEL, 100, and the largest duplicate this project has
 * found: FIVE names in five files, every one of them 0x64.
 *
 *     ...AppearanceNone    cheats.h            "no character here"
 *     ...PickPadding       characterpick.h     "the game's own not-available
 *                                              padding"
 *     ...SelectPadding     selectwidget.h      what the roster filter at
 *                                              0x0206F620 substitutes for a
 *                                              locked hunter
 *     kNoCharacter         partyportrait.cpp   free_play_pages.json's
 *                                              `empty_slot`
 *     kSelectPadding       lasthunter.cpp      "it is a ROM constant"
 *
 * THE FIRST THREE ARE WRITTEN WITH LEADING DOTS ON PURPOSE. This table listed
 * all five names in full, and then the commit that wrote it ran a bulk rename
 * over every header - so all five became kLsw2CharacterNone and the table
 * proving "one value, five names" showed one name five times. The three that
 * begin with the kLsw2 prefix are the ones a future rename would eat again, so
 * they are spelled un-greppably. The same rename damaged a comment in
 * cheats.cpp in the same commit; both are noted where they happened.
 *
 * WHY IT SPLIT FIVE WAYS. Each file met the value in a different costume - as
 * a page's tail padding, as an empty party slot, as a locked hunter's
 * stand-in, as an appearance record's empty field - and named it after the
 * costume rather than after the value. They are one fact: 100 is what this
 * game writes where a character id would go when there is no character. Five
 * names is what makes a correction to one of them invisible to the other four.
 *
 * Named for the value and not for any one of its uses. */
constexpr uint8_t kLsw2CharacterNone = 0x64u;

/* The actor pointer in a party slot, or 0. */
uint32_t lsw2_cheats_party_actor(int slot);

/* The character id in a slot, or -1 when the slot is empty. Greedo is 20. */
int lsw2_cheats_party_character(int slot);

/* THE ENHANCED ARENA CONTROLS' LEVER, read rather than assumed.
 *
 * `actor+0x5DA` is the view index. 0x0204A5BC
 * refuses to compute a camera-yaw offset when it reads 4 there - writing zero
 * instead - and a zero offset is exactly world-absolute movement, which is
 * the feel called tank controls here. Any other value sends it on to the
 * camera manager and the movement becomes camera-relative.
 *
 * So this one byte is the difference between the two control schemes, and the
 * game already contains both. What has never been measured is which value the
 * arena actually assigns - both construction sites COMPUTE it, so
 * "the arena passes 4" is an inference from the observed feel rather than a
 * reading. This is how that gets settled: one number, logged once.
 *
 * Returns -1 when the slot is empty or guest state is unreachable. */
int lsw2_cheats_party_view_index(int slot);

/* The enhanced arena controls' lever, written. See enhanced.h for why one byte is a control
 * scheme and why the port may change it without an override. Returns false when
 * the slot has no actor, which is every frame outside a level. */
bool lsw2_cheats_set_party_view_index(int slot, int value);

/* IS THE PLAYER IN AN ARENA MATCH RIGHT NOW?
 *
 * The two Bounty Hunter maps, both "confirmed" in levels.h: 4 Jabba's Tower and
 * 5 Mos Eisley Streets. The Cantina Arena Lobby (9) is deliberately NOT one of
 * them - it is where matches are arranged, not played, and several features
 * turn on exactly that distinction:
 *
 *   the match HUD     hides the touch screen during gameplay only
 *   enhanced controls apply in arena gameplay, not in the lobby, which keeps
 *                     the usual controls
 *
 * Returns false when guest state is unreachable, which is the safe answer for
 * both: neither feature should engage on a maybe. */
bool lsw2_cheats_in_arena_match();

/* Remove a party member. Returns true if it removed one, false if the slot was
 * already empty or guest state is unreachable. Slot 0 is refused - removing the
 * player is not something this knows how to do and would leave the game with
 * nobody to control. */
bool lsw2_cheats_remove_party_member(int slot);

/* Read both arrays. Returns false when guest state is unreachable. */
bool lsw2_cheats_read_flags(uint32_t* array_a, uint32_t* array_b);

/* Write both arrays back. Each pointer may be null to leave that array alone.
 *
 * Writes whole words rather than individual bits deliberately: the arrays are
 * the unit that was captured, and setting bits one at a time would reintroduce
 * exactly the per-bit guessing this avoids. */
bool lsw2_cheats_write_flags(const uint32_t* array_a, const uint32_t* array_b);

#endif /* LSW2_CHEATS_H */
