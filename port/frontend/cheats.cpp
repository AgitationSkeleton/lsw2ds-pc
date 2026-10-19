/* See cheats.h. */

#include "cheats.h"
#include "gearhop.h"
#include "statelog.h"
#include "statereq.h"        /* lsw2_state_request_in_slot */

#include "levels.h"
#include "state.h"

#include <cstdio>

namespace {

/* THE CONTEXT POINTER. How it was found: 0x0208C888 is the
 * level-enter method of a game-state class (vtable slot 2 at 0x020FF478) and
 * takes its level and mode from the object it is called on. Searching the
 * running game for that vtable pointer found the live instance, and searching
 * for a pointer to THAT found exactly one holder, at 0x02102900 - a static
 * address referenced from about 106 literal-pool sites, so the game's central
 * context rather than an incidental copy.
 *
 *     context     = *(u32*)0x02102900
 *     level index = *(u32*)(context + 0x0C)
 */
/* See statelog.h - kLsw2StateManager is the one copy of this address.
 *
 * THIS FILE HELD IT TWICE, under two names: `kContextPtr` up here, dereferenced
 * to reach slot 0's state object, and `kStateManager` further down as the base
 * of the slot structure. Same word, two spellings, one file - which is how a
 * later correction lands on one of them and not the other. */
constexpr uint32_t kContextLevelIndex = 0x0Cu;

/* Main RAM. Anything outside this is not a context pointer, and treating a
 * stale or uninitialised word as one would have us writing into whatever
 * happens to be at that address. */
constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

constexpr int kArm9 = 9;

bool ram_pointer(uint32_t value) {
    return value >= kRamLow && value < kRamHigh;
}

/* Read through the debug bus rather than the guest fast path: this runs from
 * the present loop, outside any guest instruction, which is exactly the case
 * bus_device_* exists for. */
uint32_t read32(uint32_t addr) {
    return bus_device_read32(kArm9, addr);
}

bool context(uint32_t* out) {
    if (!ram_pointer(kLsw2StateManager)) return false;
    const uint32_t ctx = read32(kLsw2StateManager);
    if (!ram_pointer(ctx)) return false;
    if (out) *out = ctx;
    return true;
}

}  // namespace

bool lsw2_cheats_available() {
    uint32_t ctx = 0;
    return context(&ctx);
}

int lsw2_cheats_current_level() {
    uint32_t ctx = 0;
    if (!context(&ctx)) return -1;
    const uint32_t index = read32(ctx + kContextLevelIndex);
    /* A plausible index or nothing. An out-of-range value means the chain
     * resolved to something that is not the context, and reporting it as a
     * level would be worse than reporting nothing. */
    return index < uint32_t(kLsw2LevelCount) ? int(index) : -1;
}

int lsw2_cheats_current_level_via_state() {
    /* THE INSTALLED STATE OBJECT, which answers a related but different
     * question - see the header. It is written by the game's ordinary level
     * entry path and left alone by the warp, so a difference from the request
     * means "put here rather than arrived here", not "an address is wrong". */
    if (!ram_pointer(kLsw2LevelRootAddr)) return -1;
    const uint32_t obj = read32(kLsw2LevelRootAddr + kLsw2LevelObjOffset);
    if (!ram_pointer(obj)) return -1;
    const uint32_t word = read32((obj + kLsw2LevelIndexOffset) & ~3u);
    const uint32_t shift = ((obj + kLsw2LevelIndexOffset) & 3u) * 8u;
    const uint32_t index = (word >> shift) & 0xFFu;
    return index < uint32_t(kLsw2LevelCount) ? int(index) : -1;
}

bool lsw2_cheats_set_level(int index) {
    if (index < 0 || index >= kLsw2LevelCount) return false;
    uint32_t ctx = 0;
    if (!context(&ctx)) return false;
    bus_device_write32(kArm9, ctx + kContextLevelIndex, uint32_t(index));
    return true;
}

/* THE STATE-REQUEST QUEUE. In short, the pump at 0x0205F45C reads a status byte per slot, and on 1 it builds
 * the state described by that slot's argument block and immediately calls the
 * new state's enter method (vtable slot 2 - the level loader).
 *
 * Every offset below was read back out of the running game and matched against
 * the instructions that use it, rather than inferred from the layout. The live
 * level-state object reads `vtable, 4, 5, 14, 0`, which is exactly what the
 * constructor at 0x0205F7F4 stores at +0x00, +0x04, +0x08, +0x0C and +0x10. */
namespace {

/* The slot structure's base - see statelog.h. */
/* THE SLOT STRIDE, which was a bare 32 in the one place that indexed by slot
 * and folded into a literal in the other. See kSlot1Args below. */
/* TWO SLOTS, not a guess: the manager's caller reads `cmp r5, #2`,
 * and the live block confirmed it - the argument blocks at +0x48 and +0x68 sit
 * below the status bytes at +0x90 precisely because there are only two. */
constexpr int kStateSlotCount = 2;
/* The pump writes 3 once it has built and entered a state. Anything else means
 * a transition is in flight. */

/* The state class the factory's 58-entry jump table builds for a level. Read
 * off the table rather than guessed: case 4 is the one whose body installs
 * vtable 0x020FF478 and stores the level index at +0x0C. */

/* The in-game touch-screen state, measured: slot 1 reads 48 in a real level and
 * 15 during the boot. */
constexpr uint32_t kGameUiKind = 48u;

/* Field offsets in the built state object, from the same constructor. */

void write32(uint32_t addr, uint32_t value) {
    bus_device_write32(kArm9, addr, value);
}

/* A SLOT THAT CAN TAKE A REQUEST: its state built (status 3) and no request
 * already waiting in its block. The id word is what the manager's transition
 * (0x0205EF84) looks for every frame and what the pump (0x0205F45C) zeroes
 * once it has built, so a non-zero id is a request still in flight - fading
 * out, or the old state being exited. The forced-status writers this file used
 * to have built on the very next pump and never had to ask. */
bool slot_takes_request(uint32_t slot) {
    const uint32_t status = read32(kLsw2StateManager + kLsw2SlotStatus);
    if (((status >> (slot * 8u)) & 0xFFu) != kLsw2StatusBuilt) return false;
    return read32(kLsw2StateManager + slot * kLsw2SlotStride + kLsw2SlotArgs) == 0u;
}

}  // namespace

bool lsw2_cheats_can_warp() {
    /* ONLY FROM A SETTLED STATE, and this is not caution for its own sake.
     *
     * Requesting a new level while one is still loading tears down a state the
     * game is in the middle of building. Doing that crashed the guest outright
     * - it carried on executing into instruction words as if they were data,
     * which is what a torn-down object with live pointers into it looks like.
     *
     * The pump leaves the status byte at 3 once it has finished; any other
     * value, or a request still waiting in the slot's block, means a transition
     * is in flight and the request must wait. */
    if (!slot_takes_request(kLsw2WorldSlot)) return false;
    const uint32_t state = read32(kLsw2StateManager + kLsw2SlotResult);
    if (!ram_pointer(state)) return false;
    return read32(state + kLsw2StateKind) == kLsw2LevelStateKind;
}

bool lsw2_cheats_warp(int index) {
    if (index < 0 || index >= kLsw2LevelCount) return false;
    if (!lsw2_cheats_can_warp()) return false;
    const uint32_t state = read32(kLsw2StateManager + kLsw2SlotResult);
    if (!ram_pointer(state)) return false;

    /* REUSE THE LIVE STATE'S OWN ARGUMENTS and change only the level. The other
     * two are whatever mode the game is currently running in; inventing values
     * for them would be guessing at a meaning that has not been established,
     * and the observed pair (5, 0) is almost certainly not universal. */
    const uint32_t kind = read32(state + kLsw2StateKind);
    if (kind != kLsw2LevelStateKind) return false;
    const uint32_t arg_a = read32(state + kLsw2StateMode);
    const uint32_t arg_c = read32(state + kLsw2StateArgC);

    /* THE GAME'S OWN REQUEST, NOT THE STATUS BYTE. This used to
     * write the four words and set the slot's status to 1 itself, which makes
     * the pump build the new level over the old one: the old level's exit
     * (vtable+0x10) and destructor (vtable+4) never ran, its registrations
     * stayed live, and the new level's load reused the memory they point at -
     * one cause of all four consoles' sprite loaders reading freed
     * sources after a match. Written as 0x0205EC4C writes it, the manager fades
     * the slot, exits and destroys the old level, and only then builds. */
    return lsw2_state_request_in_slot(kLsw2WorldSlot, kind, arg_a,
                                      uint32_t(index), arg_c);
}

/* ------------------------------------------------------------------------- */
/* The unlock flags. See cheats.h for the disassembly every constant came from. */

namespace {

/* The flag block. The dispatcher loads this address as an immediate from its
 * literal pool at 0x0207977C and passes it as the accessors' first argument, so
 * it is the block itself and not a pointer to one.
 *
 * The project already knew this address without knowing what it was:
 * config/free_play_pages.json records "page_memory" at 0x02104D90+0x51 and a
 * party-swap refusal on 0x02104D90 & 1. It is the global progress structure. */
constexpr uint32_t kFlagBlock = 0x02104D90u;

/* Offsets within it. Both accessors compute base + 0x4000 + k and index by
 * (n >> 5) * 4, so these are the array bases.
 *
 * NAMED FOR WHAT THEY HOLD, because this file declared them TWICE - here as
 * kArrayOwned and kArraySeen while they were still anonymous, and again 190
 * lines below as kArrayOwned and kArraySeen once they had been identified.
 * Same file, same values, two vocabularies, exactly the shape that had
 * dispatch_overrides.cpp holding kWhContext and kWhCtx 780 lines apart. */
constexpr uint32_t kArrayOwned = 0x44CCu;
constexpr uint32_t kArraySeen = 0x44DCu;

}  // namespace

/* The ten arrays the accessors address, with their accessor pairs.
 *
 * The offsets are certain - each comes from the ADD/LDR pair inside the
 * accessor itself. The NOTES are not all certain, and say so: only two of the
 * ten have an identity corroborated from more than one direction. */
const Lsw2FlagArray kLsw2FlagArrays[kLsw2FlagArrayCount] = {
    { 0x4488u, "mode bitfield - bit 10 Free Play mode, bit 11 "
               "multi-view. test 0x02086204, set 0x020861BC" },
    { 0x44CCu, "test 0x02086534, set 0x02086458, 104 sites, indices 0..99 over "
               "the character and vehicle ids. An earlier reading called this "
               "the unlock test; a capture says it does not differ between an "
               "unlocked and a fresh save. UNRESOLVED." },
    { 0x44DCu, "test 0x02086588, set 0x02086558" },
    { 0x44ECu, "test 0x020865DC" },
    { 0x450Cu, "test 0x02086630" },
    { 0x4514u, "test 0x0208671C" },
    { 0x4518u, "EXTRAS enabled - test 0x020867EC, set 0x02086810. SHARES AN "
               "INDEX with +0x4514: the terminal's toggle at 0x02085228 passes "
               "ONE value to both. It used to say otherwise, and that note is "
               "what kept the host's Extras unapplied. The 22-vs-18 count difference "
               "is the likeliest thing it meant." },
    { 0x451Cu, "test 0x02086E4C, set 0x02086E1C - the test REMAPS "
               "indices 22->0, 23->5, 24->10 before the bit test - and the "
               "SETTER does not, so the two use different spaces. The "
               "only accessor of the nine that remaps." },
    { 0x4520u, "test 0x02086F20" },
};

bool lsw2_cheats_read_all_flag_arrays(uint32_t* out) {
    if (!out || !lsw2_cheats_available()) return false;
    for (int a = 0; a < kLsw2FlagArrayCount; ++a)
        for (int i = 0; i < kLsw2FlagWords; ++i)
            out[a * kLsw2FlagWords + i] =
                read32(kFlagBlock + kLsw2FlagArrays[a].offset + uint32_t(i) * 4u);
    return true;
}

bool lsw2_cheats_read_flags(uint32_t* array_a, uint32_t* array_b) {
    /* GUARDED ON THE CONTEXT, not on the flag block's own contents. There is no
     * value the block cannot legitimately hold - all zeroes is a fresh save -
     * so it cannot validate itself. What it can rely on is that the game has
     * built its context, which is the same condition everything else here
     * uses. */
    if (!lsw2_cheats_available()) return false;
    for (int i = 0; i < kLsw2FlagWords; ++i) {
        if (array_a) array_a[i] = read32(kFlagBlock + kArrayOwned + uint32_t(i) * 4u);
        if (array_b) array_b[i] = read32(kFlagBlock + kArraySeen + uint32_t(i) * 4u);
    }
    return true;
}

bool lsw2_cheats_write_flags(const uint32_t* array_a, const uint32_t* array_b) {
    if (!lsw2_cheats_available()) return false;
    for (int i = 0; i < kLsw2FlagWords; ++i) {
        if (array_a) write32(kFlagBlock + kArrayOwned + uint32_t(i) * 4u, array_a[i]);
        if (array_b) write32(kFlagBlock + kArraySeen + uint32_t(i) * 4u, array_b[i]);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* THE LOBBY CONFINEMENT: the lobby room's exit into the rest of the Cantina
 * is shut off, which keeps the player inside the room.
 *
 * NOT BY FINDING THE DOOR. Two searches for the entity came up empty and
 * config/arena_entity_catalog.json names only Jabba's Barge doors.
 * What the door DOES is ask the state manager for another level, and the port
 * already speaks that protocol - so the requirement is met by refusing the
 * transition rather than by disabling an entity.
 *
 * HOW THE REFUSAL WORKS. A pending request is `status == 1` with the argument
 * block naming a kind and a level. If the kind is a level state and the level
 * is not the one we are pinned to, the LEVEL INDEX is rewritten before the pump
 * reads it. The request still runs; it just arrives back where it started.
 *
 * WHY REWRITE RATHER THAN CANCEL. Clearing the status byte would leave whatever
 * asked for the transition believing it succeeded, and the game has a door
 * animation and a fade that are already in flight by then. Reloading the lobby
 * is a transition the game performs correctly every time, because it is the one
 * the fork's own boot uses.
 *
 * THIS RACED THE PUMP, THE RACE WAS MEASURED, AND IT IS GONE. This paragraph
 * used to end by calling the race unmeasured and the choice between the two
 * approaches an open question, and the paragraph immediately below it has
 * described the answer ever since: a player walked out of the lobby with the
 * old version running every frame, and the request turned out to be posted and
 * consumed inside a single frame, so `status == REQUESTED` was a state a
 * once-per-frame poll could never observe.
 *
 * The fallback the old text proposed - warp the player back, "cruder and
 * certain" - was NOT what fixed it. Testing the ARGUMENTS instead of the status
 * removed the window rather than racing it, because the arguments name a level
 * for as long as the slot describes one. So there is no longer a choice to
 * make, and this comment no longer poses one; two paragraphs in the same
 * function disagreeing about whether a question is open is how a settled thing
 * gets re-litigated. */
bool lsw2_cheats_pin_level(int level) {
    if (!lsw2_cheats_available()) return false;

    for (int slot = 0; slot < kStateSlotCount; ++slot) {
        /* THE STATUS CHECK USED TO BE HERE AND IT LOST THE RACE.
         *
         * A player walked out of the lobby into the Cantina with this running
         * every frame. The comment above predicted exactly that: the request is
         * posted and consumed by the game's own pump inside a single frame, so
         * `status == REQUESTED` is a state a once-per-frame poll can simply
         * never observe. Waiting for it meant enforcing nothing.
         *
         * Testing the ARGUMENTS instead has no such window. They name a level
         * for as long as the slot describes one, so the check below is true from
         * the moment the destination is written until it is replaced - and while
         * the lobby is the current level, the arguments already say so and this
         * writes nothing at all. */
        const uint32_t args =
            kLsw2StateManager + uint32_t(slot) * kLsw2SlotStride + kLsw2SlotArgs;
        if (read32(args) != kLsw2LevelStateKind) continue;   /* not a level change */

        const uint32_t requested = read32(args + 8u);
        if (requested == uint32_t(level)) continue;      /* already going home */

        /* A TRANSITION TO AN ARENA IS ONE THE FORK WANTS, and refusing it would
         * have cancelled the multiplayer round start.
         *
         * The confinement asks for the exit door disabled - the way OUT of the
         * lobby, to the Cantina. It does not ask for every level change to be
         * refused, and this function refused all of them. The round start reads as
         * `0x0205ECC4(0x02102900, 4, 0, 0xF, level, 0)` - a kind-4 level request
         * carrying an arena - and it is issued WHILE THE PLAYER IS STANDING IN
         * THE LOBBY, which is exactly when the caller runs this.
         *
         * So the pin would have rewritten the arena back to the lobby every
         * time, once a frame, and the match would simply never have started.
         * The failure would have looked like the round-start message doing
         * nothing, and the search would have begun in the netplay code.
         *
         * The caller's guard does not help: it skips the pin when the CURRENT
         * level is not the lobby, and during a round start the current level is
         * still the lobby. It was written against the opposite hazard - being
         * trapped in the arena afterwards - which it does prevent. */
        if (lsw2_level_is_arena(int(requested))) continue;

        /* SAY WHAT WAS REWRITTEN, because this is a silent override of the
         * game's own decision and silence has cost this project a day.
         *
         * The pin exists to disable the lobby's exit door, and it does that
         * by rewriting the destination of any pending level request that is not
         * the lobby and not an arena. That is a big hammer, and while two
         * consoles are being joined into a wireless session the game issues
         * level requests of its own. If one of those is rewritten, the child
         * reloads the lobby instead of doing whatever the session asked for, and
         * nothing anywhere says so.
         *
         * ONCE PER DESTINATION, not once per frame: the pin runs every tick and
         * a request can sit for several, so an unconditional line would bury the
         * log while a per-destination one records every distinct override. */
        static uint32_t said = 0xFFFFFFFFu;
        if (said != requested) {
            said = requested;
            std::fprintf(stderr,
                         "[fork] level pin: the game asked for level %u and the "
                         "fork rewrote it to %d (the lobby door refusal). If a "
                         "wireless session was being set up, that request was "
                         "the game's, not the door's.\n",
                         unsigned(requested), level);
        }

        write32(args + 8u, uint32_t(level));
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* EVERYTHING UNLOCKED FROM THE START, for the fork.
 *
 * Save-file flags are set so that every character, every customization
 * part and weapon, and every Extra is unlocked.
 *
 * WHAT IS WRITTEN AND WHY EACH ONE. The Cantina terminal - the game's own
 * shop - writes two arrays per character:
 *
 *   +0x44CC  OWNED   the character is bought
 *   +0x44DC  SEEN    the character is known; the cell draws its NAME
 *
 * BOTH ARE REQUIRED. The Characters tab tests SEEN first and draws nothing when
 * it is clear, which is what string 0x294 "????" is for - so writing only OWNED
 * gives a roster where everything is bought and everything still reads as
 * question marks. That is worse than doing nothing, and it is the specific
 * half-correct write this was designed to avoid.
 *
 * ONLY THE INDICES THE GAME SHIPS, never 0xFFFFFFFF. A census of the ids the
 * game actually tests found: 0..73 with gaps, then 85..99. Id 73 is
 * "XXXDO NOT USE Custom 1" and 74-84 are the unused custom slots after it; 85
 * onward are vehicles. Bits the game never tests are bits whose meaning nobody
 * knows, and a save with unknown bits set behaves in ways nobody predicted.
 */
namespace {

/* From a census of the indices passed to the accessors, PLUS the seven the
 * fork's own character picker offers that the census missed.
 *
 * WHY THAT ADDITION IS SAFE, WHICH THE COMMENT ABOVE IS RIGHT TO DEMAND.
 * The rule this file states is "bits the game never tests are bits whose
 * meaning nobody knows". These seven are not that. `kLsw2PickTable` in
 * characterpick.cpp OFFERS ids 3, 4, 5, 6 (page 9), 20 (page 6), 35 (page 0)
 * and 56 (page 1) to the player, and id 20 is the fork's DEFAULT slot-0
 * character - the log line reads "party slots now 20 100 100 100".
 *
 * And the live save settles it. Read after this function had already run:
 *
 *     id 20   OWNED = 1   SEEN = 0        id 36  OWNED = 1  SEEN = 1
 *     id 35   OWNED = 1   SEEN = 0
 *     id 56   OWNED = 1   SEEN = 0
 *
 * The game's own save ALREADY OWNS 20, 35 and 56. Their OWNED bit is set by
 * something other than this list, so they are characters the game tests; only
 * their SEEN companion was missing, and SEEN clear is exactly what draws string
 * 0x294 "????" - the bug that showed as `ndsrecomp - ????`.
 *
 * 3, 4, 5 and 6 are the mirror case: SEEN already set, OWNED clear. Offered by
 * the picker and not bought, which "all characters unlocked" requires fixing.
 *
 * Deliberately NOT extended to every id with a clear bit. 19, 30, 45, 49, 55,
 * 57 and 72-84 are unreachable through the picker and nothing has shown the
 * game uses them, so they stay untouched. */
const uint8_t kOwnableIds[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 31, 32, 33, 34, 35,
    36, 37, 38, 39, 40, 41, 42, 43, 44, 46, 47, 48,
    50, 51, 52, 53, 54, 56, 58, 59, 60, 61, 62, 63, 64, 65, 66,
    67, 68, 69, 70, 71, 73,
    85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99,
};

constexpr uint32_t kArrayBought = 0x4514u;   /* terminal purchases */
constexpr uint32_t kArrayExtrasEnabled = 0x4518u;  /* which Extras are ON */
constexpr int kExtrasBoughtCount = 22;       /* the Extras tab's option count */

/* THE TWO ARRAYS THE CHARACTER CUSTOMIZE SCREEN GATES ITS ROWS ON.
 *
 * FOUND BY THE BUG, NOT BY A SCAN, which is why the numbers below are exact
 * rather than a range. The customize menu was seen offering three options - a
 * hat, sunglasses and a moustache - where every part the game has should be
 * offered. The widget's constructor at 0x0207A9C4 explains it
 * completely: it builds its row list from an 8-byte record table, and each
 * record's first byte says which array gates it.
 *
 *     kind 0   always shown                            1 row
 *     kind 3   +0x4514 index 11  ("Moustaches" Extra)  1 row
 *     kind 4   +0x4514 index 10  ("Sunglasses" Extra)  1 row
 *     kind 1   +0x450C [record byte 1]                17 rows
 *     kind 2   +0x44EC [record byte 1]                 9 rows
 *
 * Three of those gates were already satisfied - the unconditional row, and the
 * two Extras that lsw2_cheats_unlock_everything already buys. THE THREE ROWS
 * THE PLAYER SAW WERE EXACTLY THE THREE THIS CODE UNLOCKED, and the other
 * twenty-six were the two arrays it did not write.
 *
 * The table has two forms and the widget picks between them on +0x4488 bit 10,
 * the same flag the dispatch override already answers "yes" to: twelve rows at
 * 0x020EE6A4 in party mode, twenty-nine at 0x020EE704 in roster mode. The
 * twenty-nine are a superset, so the indices below are read from that one.
 *
 * WHAT THE TWO ARRAYS ARE. +0x44EC is indexed by ACTOR PART ID, and seven of
 * its nine indices are the authored headwear block that
 * config/customization_modes.json already names - 178 BaseballCap, 187
 * CowboyHat, 188 Crown, 202 KnightHelmet, 214 PirateHat, 228 TopHat, 229
 * WizardHat. That is a strong independent check on the reading: an array whose
 * indices land on seven named hats is the array that says which parts you own.
 * 205 and 208 are two more parts in the same block that nothing has named yet.
 *
 * +0x450C's index space is NOT the part space - 7..33, dense - and nothing
 * outside this screen consumes it, so what it counts over is not established.
 * It does not need to be: every row is to be offered, and these are
 * the seventeen indices the seventeen rows ask about.
 *
 * ONLY THE INDICES THE GAME TESTS, for the same reason kOwnableIds is a list
 * rather than a range. Both arrays have room for more - the gap to the next
 * known array is 32 bytes for +0x44EC and 8 for +0x450C - and setting bits no
 * accessor ever reads would be setting bits whose meaning nobody knows. */
constexpr uint32_t kArrayPartsOwned = 0x44ECu;
constexpr uint32_t kArrayCustomiseB = 0x450Cu;

/* Record byte 1 of every kind-2 row of 0x020EE704. Actor part ids. */
const uint8_t kCustomisePartIds[] = {
    178, 187, 188, 202, 205, 208, 214, 228, 229,
};

/* Record byte 1 of every kind-1 row of 0x020EE704, sorted. */
const uint8_t kCustomiseOtherIds[] = {
    7, 8, 9, 10, 14, 15, 17, 18, 19, 21, 22, 23, 24, 25, 29, 30, 33,
};

/* Set one bit in one array, read-modify-write, the way the guest's own setters
 * do it. Word index is bit >> 5 exactly as every accessor computes it. */
void set_flag(uint32_t array_offset, unsigned bit) {
    const uint32_t addr = kFlagBlock + array_offset + (bit >> 5) * 4u;
    write32(addr, read32(addr) | (1u << (bit & 31u)));
}

void put_flag(uint32_t array_offset, unsigned bit, bool on) {
    const uint32_t addr = kFlagBlock + array_offset + (bit >> 5) * 4u;
    const uint32_t mask = 1u << (bit & 31u);
    const uint32_t w = read32(addr);
    write32(addr, on ? (w | mask) : (w & ~mask));
}

}  // namespace

/* THE HOST'S EXTRAS, actually switched on.
 *
 * `+0x4518` is the ENABLED array - `0x020867EC` tests it, `0x02086810` sets it -
 * as distinct from `+0x4514`, which is what the Cantina terminal has SOLD you.
 * The unlock fills the second and deliberately not the first, because which Extras are
 * on is the host's decision, not the unlock's.
 *
 * THE TWO ARRAYS SHARE AN INDEX, which is worth stating because a note in this
 * file said the opposite ("NOT the same index space") and that note is what kept
 * this unbuilt. The Extras terminal's own toggle handler settles it: at
 * `0x02085228` it takes ONE value, `[r10+0x1C]`, and passes it to
 * `0x02086740` - which begins by testing `+0x4514` with it - and then to
 * `0x02086810`, which sets `+0x4518` with it. One index, both arrays.
 *
 * AND THE COUNT IS 18. `0x020867A4` is the game's own clear-all loop and it runs
 * `while (i < 0x12)`, which is exactly `kLsw2ExtrasCount`. The bought array
 * holds 22 - the terminal sells more than it can switch on - and that difference
 * is the likeliest thing the old note was reaching for.
 *
 * WRITTEN, NOT MESSAGED. The cartridge broadcasts message 0x36 per extra when a
 * session is up and writes the array directly when one is not (the same handler,
 * both arms). The fork writes directly on every machine, because the host's
 * settings already travel over the fork's own transport - so each client can
 * apply what it was told without depending on the cartridge's pump being live,
 * which is the one thing the whole message layer is still gated on. */
bool lsw2_cheats_request_appearance(int slot) {
    if (slot < 0 || slot >= 4) return false;
    if (!lsw2_cheats_available()) return false;

    const uint32_t rec =
        kLsw2AppearanceRecord + uint32_t(slot) * kLsw2AppearanceStride;

    /* AN EMPTY SLOT IS NOT A REQUEST. `0x02089858` tests the character against
     * 0x64 before it looks at the gate at all, so gating a slot with no
     * character in it asks for nothing and just leaves a byte set for whoever
     * spawns there next. */
    const uint32_t head = read32(rec);
    if ((head & 0xFFFFu) == kLsw2CharacterNone) return false;

    /* THE GATE, a byte, read-modify-written through its word - the record's
     * neighbours are live fields and 0x1B is not word aligned. */
    const uint32_t addr = rec + kLsw2AppearanceGate;
    const uint32_t base = addr & ~3u;
    const uint32_t sh = (addr & 3u) * 8u;
    const uint32_t w = read32(base);
    if (((w >> sh) & 0xFFu) != 0u) return true;   /* already pending */
    write32(base, (w & ~(0xFFu << sh)) | (1u << sh));
    return true;
}

bool lsw2_cheats_set_extras(const bool* enabled, int count) {
    if (!enabled || !lsw2_cheats_available()) return false;
    if (count > kLsw2ExtrasEnabledCount) count = kLsw2ExtrasEnabledCount;

    /* EVERY BIT, NOT JUST THE ONES THAT ARE ON. Turning an Extra off in the
     * host menu has to turn it off in the game, and a set-only pass would make
     * Extras accumulate for the rest of the session - which is exactly the
     * failure the game's own clear-all loop exists to avoid. */
    for (int i = 0; i < count; ++i)
        put_flag(kArrayExtrasEnabled, unsigned(i), enabled[i]);
    return true;
}

/* The cartridge's own Extras toggle - see cheats.h. */
constexpr uint32_t kLsw2ExtrasToggle = 0x02086810u;

bool lsw2_cheats_toggle_extra_if_needed(int i, bool wanted) {
    if (i < 0 || i >= kLsw2ExtrasEnabledCount) return false;
    if (!lsw2_cheats_available()) return true;   /* not ready - still pending */

    const uint32_t addr =
        kFlagBlock + kArrayExtrasEnabled + (unsigned(i) >> 5) * 4u;
    const bool cur = ((read32(addr) >> (unsigned(i) & 31u)) & 1u) != 0u;
    if (cur == wanted) return false;             /* already right */

    /* It differs: ask the game to flip it, on the guest thread. rebuild=1 so
     * the party re-dresses into the new theme. A refusal (queue full) leaves it
     * pending for the next frame. */
    lsw2_gearhop_request_toggle(kLsw2ExtrasToggle, kFlagBlock, unsigned(i), 1u);
    return true;
}

int lsw2_cheats_extra_enabled(int i) {
    if (i < 0 || i >= kLsw2ExtrasEnabledCount) return -1;
    if (!lsw2_cheats_available()) return -1;
    const uint32_t addr =
        kFlagBlock + kArrayExtrasEnabled + (unsigned(i) >> 5) * 4u;
    return int((read32(addr) >> (unsigned(i) & 31u)) & 1u);
}

bool lsw2_cheats_unlock_everything() {
    if (!lsw2_cheats_available()) return false;

    for (uint8_t id : kOwnableIds) {
        set_flag(kArrayOwned, id);
        set_flag(kArraySeen, id);
    }

    /* THE EXTRAS, BOUGHT - not enabled. +0x4514 is what the terminal sells and
     * +0x4518 is what is switched on. They are unlocked here and the switching
     * is handed to the host's match settings, so setting the enable array
     * would take that decision away from the host. */
    for (int i = 0; i < kExtrasBoughtCount; ++i)
        set_flag(kArrayBought, unsigned(i));

    /* THE CUSTOMIZATION PARTS - every part and weapon the customize screen
     * offers, and the half of the unlock that was missing until playing the
     * game found it. See the tables above for what each array gates. */
    for (uint8_t id : kCustomisePartIds) set_flag(kArrayPartsOwned, id);
    for (uint8_t id : kCustomiseOtherIds) set_flag(kArrayCustomiseB, id);

    return true;
}

/* ------------------------------------------------------------------------- */
/* The party. See cheats.h for where each address came from and for the note that
 * this behaviour is the port's own and has no counterpart in the ROM. */

namespace {

/* The level root. An earlier claim was that this held the level
 * id directly; it does not - it is the root object whose +0x68 is the installed
 * level state and whose +0x4C begins the party array. */
/* THE LEVEL ROOT, from the GENERATED levels.h as kLsw2LevelRootAddr - one
 * copy, and the right one: extract_level_table.py regenerates it from the
 * ROM, so a re-extraction updates every consumer instead of leaving
 * hand-written copies behind. The +0x4C array offset and the character byte
 * are cheats.h's kLsw2PartyActors and kLsw2ActorCharacter; this file used to
 * declare both again, as kPartyBase and kActorCharId.
 *
 * THOSE TWO OLD NAMES ARE WRITTEN OUT IN FULL BECAUSE THE RENAME ATE THEM
 * ONCE. The commit that moved these constants ran a bulk rename over the file
 * AFTER adding this comment, so `kPartyBase` and `kActorCharId` were rewritten
 * to the new names and the sentence came to say the file used to declare them
 * under the names it declares them under now - true of nothing, and the exact
 * hazard the project had already written down. Say the old name last, or say
 * it after the rename. */

/* The flags word the game's own presence test reads:
 *     present  <=>  (actor[0x08] & 0x803) == 1
 * so clearing bit 0 makes every site that asks "is this member here" say no. */
constexpr uint32_t kActorFlags = 0x08u;
/* Earlier readings called it the controller index and the view index; both
 * are the same byte, and 4 is its "no view of my own" sentinel. */
constexpr uint32_t kActorViewIndex = kLsw2ActorViewIndex;
constexpr uint32_t kActorPresentBit = 1u;

}  // namespace

uint32_t lsw2_cheats_party_actor(int slot) {
    if (slot < 0 || slot >= int(kLsw2PartySlots)) return 0;
    if (!lsw2_cheats_available()) return 0;
    const uint32_t p = read32(kLsw2LevelRootAddr + kLsw2PartyActors + uint32_t(slot) * 4u);
    return ram_pointer(p) ? p : 0;
}

int lsw2_cheats_party_character(int slot) {
    const uint32_t actor = lsw2_cheats_party_actor(slot);
    if (!actor) return -1;
    /* A BYTE, read as a byte. The bus helpers offer 32-bit reads, so take the
     * word and mask - reading four bytes and using all of them would give the
     * neighbouring fields, which is the width mistake playbook/06 keeps
     * recording. */
    return int(read32(actor + (kLsw2ActorCharacter & ~3u)) >> ((kLsw2ActorCharacter & 3u) * 8) & 0xFFu);
}

int lsw2_cheats_party_view_index(int slot) {
    const uint32_t actor = lsw2_cheats_party_actor(slot);
    if (!actor) return -1;
    /* A BYTE, read as a byte - the same width discipline as the character id
     * above. playbook/06 records this as a repeat offender: reading the word
     * and using all of it returns the neighbouring fields. */
    return int((read32(actor + (kActorViewIndex & ~3u)) >>
                ((kActorViewIndex & 3u) * 8)) & 0xFFu);
}

bool lsw2_cheats_set_party_view_index(int slot, int value) {
    const uint32_t actor = lsw2_cheats_party_actor(slot);
    if (!actor) return false;

    /* A BYTE, WRITTEN AS A BYTE - read-modify-write on the containing word.
     * playbook/06 names the width trap as a repeat offender and it bites twice
     * as hard here: `+0x5DA` is not word aligned, so a careless word store
     * would take three neighbouring bytes of the actor with it. */
    const uint32_t base = actor + (kActorViewIndex & ~3u);
    const uint32_t sh = (kActorViewIndex & 3u) * 8u;
    const uint32_t w = read32(base);
    write32(base, (w & ~(0xFFu << sh)) | ((uint32_t(value) & 0xFFu) << sh));
    return true;
}

bool lsw2_cheats_in_arena_match() {
    /* ONE DEFINITION OF "is this an arena", shared with the lobby pin.
     *
     * This function open-coded `level == 4 || level == 5` and `cheats.h` grew
     * `lsw2_level_is_arena()` for the pin - two ways to ask one question, which
     * is exactly the shape `lsw2_menuhost_open_p()` was deleted for. They agreed
     * on the day the second was written and had no reason to keep agreeing: the
     * moment a third arena map is confirmed, one of them gets updated.
     *
     * THE NAME IS STILL OPTIMISTIC and deliberately left so. This says the
     * player is ON an arena map, which is not the same as a round being in
     * progress - the lobby countdown, the statue select and the score screen all
     * happen elsewhere, but a match that has ended and not yet returned is still
     * "on an arena map". `[0x02107860] + 0x50` is the game's own answer
     * and its writer is unlocated, so this remains the honest
     * approximation rather than a wrong precision. */
    return lsw2_level_is_arena(lsw2_cheats_current_level());
}

bool lsw2_cheats_remove_party_member(int slot) {
    /* NEVER SLOT 0. That is the player. */
    if (slot <= 0 || slot >= int(kLsw2PartySlots)) return false;
    const uint32_t actor = lsw2_cheats_party_actor(slot);
    if (!actor) return false;

    /* ORDER MATTERS: the actor is reached THROUGH the slot pointer, so the
     * pointer has to be cleared second or there is nothing left to find. */
    const uint32_t flags = read32(actor + kActorFlags);
    write32(actor + kActorFlags, flags & ~kActorPresentBit);
    write32(kLsw2LevelRootAddr + kLsw2PartyActors + uint32_t(slot) * 4u, 0);
    return true;
}

/* ------------------------------------------------------------------------- */
/* THE COLD WARP - the fork launches straight into the lobby "instantly", not
 * "fast-forwarded".
 *
 * lsw2_cheats_warp copies two of its arguments from the level state that is
 * already running, which is why it needs one to exist. That is the right
 * conservative choice for a cheats menu used mid-game, and it is the wrong one
 * for the fork's boot: waiting for a level state means waiting for the player
 * to be put in a level, which means sitting through the title, the save select
 * and the Cantina. That is a fast-forward through the boot, not a launch
 * straight into the lobby.
 *
 * This asks for a level state COLD, with the two unknown arguments supplied
 * rather than copied. Their values are not invented - (5, 0) is what the live
 * state carried in the Cantina hub, read out of the running game:
 *
 *     slot 0 args at +0x48 = 4, 5, 0x0E, 0
 *                            ^  ^  ^     ^
 *                       kind |  |  level |  argC
 *                          argA           argC
 *
 * THIS IS STILL A GUESS ABOUT MEANING, and is labelled as one. What is known is
 * that these values produce a working level when the game itself uses them. If
 * a level ever behaves oddly on a cold warp and correctly on a warm one, this
 * pair is the first thing to suspect. */
bool lsw2_cheats_warp_cold(int index) {
    /* THE MODE THIS HAS ALWAYS WRITTEN, kept so nothing that works changes.
     * See lsw2_cheats_warp_cold_as for why the mode turned out to matter. */
    return lsw2_cheats_warp_cold_as(index, 5u);
}

bool lsw2_cheats_warp_cold_as(int index, uint32_t mode) {
    if (index < 0 || index >= kLsw2LevelCount) return false;
    if (!ram_pointer(kLsw2StateManager)) return false;

    /* THE MANAGER MUST EXIST AND BE IDLE. Its status byte reads 3 once it has
     * built whatever it last built; requesting while a build is in flight is
     * what tore down a half-constructed state and crashed the guest. Early in
     * boot the manager's memory is zeroes, and 0 is not 3, so this refuses
     * until the state machine is actually alive - which is the whole guard. */
    if (!slot_takes_request(kLsw2WorldSlot)) return false;

    /* AS THE GAME ASKS, so whatever the slot holds - the splash at boot, the
     * front end after a Disconnect - is exited and destroyed before the level
     * is built (see lsw2_cheats_warp). An empty slot is simply
     * built: 0x0205EF84 skips the exit and destructor when there is no state. */
    return lsw2_state_request_in_slot(kLsw2WorldSlot, kLsw2LevelStateKind, mode,
                                      uint32_t(index), 0u);
}

/* Whether the manager is alive and idle - the only precondition the cold warp
 * has, so the caller can poll for the earliest moment it can act. */
bool lsw2_cheats_manager_idle() {
    if (!ram_pointer(kLsw2StateManager)) return false;
    return slot_takes_request(kLsw2WorldSlot);
}

/* REPLACE THE UI STATE IN SLOT 1 WITH THE IN-GAME ONE.
 *
 * The manager has two slots. Slot 0 carries the splash, a menu or a level;
 * slot 1 carries the state that owns the touch screen. Measured across two
 * runs, and the difference is the whole answer:
 *
 *     cold-warped fork, in the Arena Lobby   slot1 kind = 15
 *     baseline driven into a level           slot1 kind = 48
 *
 * So 15 is the BOOT UI - the one that draws "LICENSED BY NINTENDO" - and 48 is
 * the in-game one. A cold warp replaces slot 0 and leaves slot 1 at 15, which
 * is exactly why the lobby had a splash on its touch screen.
 *
 * An earlier version of this function re-requested slot 1 with its OWN current
 * kind, which rebuilt the boot UI and changed nothing. The kind has to be the
 * destination, not the origin.
 *
 * The registers say the same thing from the other side. In the fork, sub-engine
 * DISPCNT_B read 0x40011113 - BG0 and objects only, mode 3 - while a real level
 * had 0x00011F10, all four backgrounds enabled, and BG3CNT_B was not merely
 * disabled but unconfigured (0 against 7184). The touch UI had never run its
 * setup, so forcing the enable bits from the host would have shown layers with
 * nothing loaded in them. */
bool lsw2_cheats_install_game_ui() {
    if (!ram_pointer(kLsw2StateManager)) return false;

    /* Both slots idle. Same hazard as the level warp: requesting into a slot
     * mid-transition tears down something the game is still building. */
    if (!slot_takes_request(kLsw2WorldSlot) || !slot_takes_request(kLsw2ScreenSlot))
        return false;

    const uint32_t state = read32(kLsw2StateManager + 4u);
    if (!ram_pointer(state)) return false;
    if (read32(state + kLsw2StateKind) == kGameUiKind) return false;  /* already */

    /* Slot 1's request. Its three trailing arguments read zero in BOTH
     * captures - the boot UI and the in-game one - so zero is measured here
     * rather than assumed. The slot is NAMED, not folded into an offset: this
     * once wrote `0x68u` (1 * 0x20 + 0x48 with the slot arithmetic done), which
     * a correction to kLsw2SlotArgs would have left behind.
     *
     * AS THE GAME ASKS, as the lobby return asks for the same 48 in
     * the same slot: the boot UI - or, after a Disconnect, the front end's -
     * is exited and destroyed before 48 is built, rather than left allocated
     * with its registrations live under the lobby's touch screen. */
    return lsw2_state_request_in_slot(kLsw2ScreenSlot, kGameUiKind, 0u, 0u, 0u);
}

int lsw2_cheats_state_kind(int slot) {
    if (slot < 0 || slot > 1) return -1;
    if (!ram_pointer(kLsw2StateManager)) return -1;
    const uint32_t p = read32(kLsw2StateManager + uint32_t(slot) * 4u);
    if (!ram_pointer(p)) return -1;
    return int(read32(p + kLsw2StateKind));
}
