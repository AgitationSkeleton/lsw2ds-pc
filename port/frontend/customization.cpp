/* See customization.h. */

#include "customization.h"
#include "vwh.h"
#include "levels.h"

#include "cheats.h"
#include "lasthunter.h"
#include "forksession.h"
#include "partslot.h"
#include "ui.h"

#include "state.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr int kArm9 = 9;
/* The slot layout was hand-read from the applier's call sites. */
/* See customization.h - kLsw2CustomBase and kLsw2CustomSlotOffsets are the
 * one copy of these. */

/* HOW LONG TO IGNORE THE GUEST AFTER A SWITCH.
 *
 * The design asks for the re-apply "a frame after the switch". That single
 * frame is also the window in which the guest has already written the NEW
 * character's stock parts over the slots - so anything read during it is the
 * stock set, not the player's choice. Adopting it would silently erase the
 * customization the moment the player changed character, which is the exact
 * failure the re-apply is meant to prevent.
 *
 * So the switch opens a short blackout: re-apply on the next frame, and do not
 * believe what is in the slots until a few frames after that. */
constexpr int kReapplyDelay = 1;
constexpr int kBlackoutFrames = 8;

uint16_t g_saved[kLsw2CustomSlots] = {kLsw2CustomNone, kLsw2CustomNone,
                                      kLsw2CustomNone, kLsw2CustomNone,
                                      kLsw2CustomNone, kLsw2CustomNone};
bool g_have_saved = false;
bool g_dirty = false;

/* THE TWO "EXTRAS" OVERLAYS the customize menu can toggle, kept beside g_saved
 * because they persist and save the same way - but they are NOT slots. The menu
 * offers a moustache row and a sunglasses row; its commit dispatcher
 * FUN_0207AEF4 handles them as row types 3 and 4 by setting bit 0x400 / 0x800 of
 * the u16 flags word at actor+0x1A8 (RE 2026-09-18, ghidra FUN_0207aef4), and
 * Reset (type 0) clears both. They are booleans, not part ids: there is no
 * per-row "off", only Reset, so each is simply on or off. */
bool g_moustache = false;
bool g_sunglasses = false;

int g_last_character = -1;
int g_reapply_in = -1;    /* frames until the re-apply, -1 when idle */
int g_blackout = 0;       /* frames during which the slots are not believed */

/* What the slots held on the frame the re-apply fires, so the report can say
 * whether the game had already replaced them with the new character's stock
 * parts. Captured rather than re-read, because the write happens first. */
uint16_t g_before[kLsw2CustomSlots] = {0, 0, 0, 0, 0, 0};

/* A handful, not every switch. The point is to characterise the mechanism, and
 * the fourth identical report says nothing the first did not. */
constexpr int kApplyReports = 4;
int g_apply_reports = 0;

/* HOW LONG TO KEEP THE SLOTS PINNED after a character switch. Half a second at
 * 60fps - comfortably longer than a spawn, short enough that the customize
 * screen is never fought with, because reaching it takes far longer than that. */
constexpr int kHoldFrames = 30;
int g_hold = 0;

uint16_t read_slot(int i) {
    /* bus_device_read32 is the only width the bus offers upward of 16 bits;
     * the slots are halfword-aligned and packed, so one word covers two of
     * them only when they are adjacent - read the containing word and pick. */
    const uint32_t addr = kLsw2CustomBase + kLsw2CustomSlotOffsets[i];
    const uint32_t word = bus_device_read32(kArm9, addr & ~3u);
    return uint16_t((addr & 2u) ? (word >> 16) : word);
}

void write_slot(int i, uint16_t value) {
    bus_device_write16(kArm9, kLsw2CustomBase + kLsw2CustomSlotOffsets[i], value);
}

/* WHAT THE HOST IS ALLOWING RIGHT NOW.
 *
 * The host controls whether customization is permitted IN MATCHES - so offline, in your own lobby, your own choices stand. The moment
 * you are in someone's session the host's row decides.
 *
 * FOR A CLIENT THIS *IS* THE HOST'S VALUE, and this comment used to say the
 * opposite. It read: "nothing sends the host's settings down the wire yet ...
 * that is Phase C's handshake", which was true when written and stopped being
 * true when the handshake landed. forksession.cpp's join path calls
 * `lsw2_host_settings_from_wire` into the SAME `settings_` this reads, under a
 * comment headed "THE CLIENT ADOPTS THE HOST'S RULES", and prints the adopted
 * customization value. So `s.settings().customization` is the host's choice on
 * a client and the client's own only while hosting or offline.
 *
 * Corrected 2026-08-30. A stale comment here is worse than none: it tells a
 * reader that the host's authority over customization (the host deciding
 * whether it is permitted at all) is unfinished, which would send them to re-implement a handshake
 * that already exists. ui.cpp relies on the same adoption from the other side -
 * it refuses to PERSIST settings while joined, precisely because `settings()`
 * then holds somebody else's rules. */
Lsw2CustomPolicy effective_policy() {
    const Lsw2ForkSession& s = lsw2_fork_session();
    if (s.state() == Lsw2SessionState::Offline) return Lsw2CustomPolicy::All;
    switch (s.settings().customization) {
    case Lsw2HostCustomization::Off:  return Lsw2CustomPolicy::Off;
    case Lsw2HostCustomization::Hats: return Lsw2CustomPolicy::Hats;
    case Lsw2HostCustomization::All:  break;
    }
    return Lsw2CustomPolicy::All;
}

/* The set to actually write: what the player chose, filtered by what the host
 * allows. g_saved stays RAW so that a host relaxing the rule mid-session
 * restores the player's choices rather than having quietly destroyed them. */
void effective_parts(uint16_t* out) {
    lsw2_customization_filter(g_saved, out, effective_policy());
}

}  // namespace

void lsw2_customization_effective(uint16_t* out) {
    if (out) effective_parts(out);
}

namespace {

/* WHAT WE LAST PUT IN THE SLOTS, so the adopt path can tell the player's doing
 * from its own. Without this the port reads back its own writes and believes
 * them - see the note over the adopt below, which is a regression this file
 * shipped with for exactly one tick. */
bool g_have_written = false;
/* WHICH SLOTS write_effective() DELIBERATELY LEFT ALONE, so the report can say
 * "kept" rather than printing an intent that never reached memory. The old line
 * printed g_saved under the heading "wrote", which was accurate only while the
 * port wrote every slot unconditionally; leaving it would have produced a log
 * claiming 00FA had been written to slots that still read 0000, which is the
 * exact species of misleading diagnostic this file has been bitten by twice. */
bool g_write_skipped[kLsw2CustomSlots] = {false, false, false,
                                          false, false, false};

/* The five wearable slots. The sixth saved entry is the WEAPON, which the
 * Reset button does not clear and which must not be counted when asking
 * "has everything gone empty" - gearapply.cpp splits them the same way. */
constexpr int kWearableSlots = 5;

/* THE ONE SLOT THE CUSTOMIZE MENU CAN DRESS: slot 3, headgear. The menu never
 * touches slots 0, 1, 2 or 4 (head, torso, legs, accessory), so the fork holds
 * those permanently empty - that is what makes a character's stock head/body/
 * helmet impossible to capture and carry forward. */
constexpr int kHeadgearSlot = 3;

/* Read a byte / halfword from guest RAM at an arbitrary address (the actor's
 * customize fields), the same way headfix.cpp does. */
uint8_t read8(uint32_t addr) {
    const uint32_t w = bus_device_read32(kArm9, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}
uint16_t read16(uint32_t addr) {
    const uint32_t w = bus_device_read32(kArm9, addr & ~3u);
    return uint16_t((addr & 2u) ? (w >> 16) : (w & 0xFFFFu));
}
void write16(uint32_t addr, uint16_t v) {
    const uint32_t base = addr & ~3u;
    const uint32_t shift = (addr & 2u) * 8u;   /* 0 or 16 */
    const uint32_t w = bus_device_read32(kArm9, base);
    bus_device_write32(kArm9, base,
                       (w & ~(0xFFFFu << shift)) | (uint32_t(v) << shift));
}

/* RE-ASSERT THE CHOSEN EXTRAS ON THE LIVE ACTOR, once a frame and idempotently.
 *
 * The actor tick FUN_02004034 reads bits 0x400 (moustache) and 0x800
 * (sunglasses) of the u16 at actor+0x1A8 EVERY FRAME and shows/hides sub-part 10
 * / 11 to match (RE 2026-09-18) - so the bit itself is the persistent state the
 * render honours, exactly like the head-reveal visibility bit in headfix.cpp,
 * and no rebuild call is needed. A character switch builds a fresh actor with the
 * bit clear, so re-asserting the player's choice here is what carries a moustache
 * or sunglasses to the switched character, the same as write_effective() carries
 * the hat and weapon.
 *
 * ONLY-SET, NEVER-CLEAR, matching both the menu (its rows only turn a bit on;
 * only Reset turns them off, and Reset is captured as the flags going false) and
 * the "nothing chosen is not wear-nothing" rule the slots follow: an actor whose
 * bit the player never set is left as the game built it.
 *
 * 0x1A8 is an inline literal for the reason headfix.cpp keeps 0xE0/0x14 inline -
 * a member offset that collides with unrelated small offsets, and the
 * dup-constants baseline is kept deliberately minimal. */
void apply_overlays() {
    if (!g_moustache && !g_sunglasses) return;   /* nothing to assert - and skip the read */
    /* Extras are the "All" tier of the host policy, not hats: a host set to Off
     * or Hats does not get a moustache or sunglasses. */
    if (effective_policy() != Lsw2CustomPolicy::All) return;

    const uint32_t actor = lsw2_cheats_party_actor(0);
    if (actor < 0x02000000u || actor >= 0x02400000u) return;

    const uint32_t addr = actor + 0x1A8u;   /* the u16 Extras flags word */
    const uint16_t flags = read16(addr);
    uint16_t want = flags;
    if (g_moustache)  want |= 0x400u;
    if (g_sunglasses) want |= 0x800u;
    if (want == flags) return;               /* already set - write nothing */
    write16(addr, want);

    static int reports = 0;
    constexpr int kOverlayReports = 4;
    if (reports < kOverlayReports) {
        ++reports;
        std::fprintf(stderr,
                     "[fork] customize: re-asserted extras on the live actor "
                     "(moustache %d, sunglasses %d; %04X -> %04X)\n",
                     int(g_moustache), int(g_sunglasses), unsigned(flags),
                     unsigned(want));
    }
}

/* "NOTHING CHOSEN" IS NOT "WEAR NOTHING", and writing it as though it were is
 * what broke the switched character's appearance.
 *
 * The symptom: characters switched to still did not inherit the customization
 * settings. The log says why outright. On a save with no customization at all,
 * g_saved is kLsw2CustomNone in every wearable slot, and this function stamped
 * that over the slots the game had just filled in:
 *
 *     [fork] customization write to character 29:
 *            was    0000 0000 0000 0000 0000
 *            wrote  00FA 00FA 00FA 00FA 00FA
 *
 * and the applier's own per-slot reporting shows what that cost, before and
 * after the same write:
 *
 *     [0] part   0 -> slot 0; the actor wears 24 there
 *     [0] part 250 -> no descriptor (>= 0xFA)
 *
 * So a 0 there resolves to a real part and the character wears it; a 0xFA
 * resolves to nothing. The port was not failing to apply the player's choices -
 * it was actively removing the character's own, every time it switched.
 *
 * THE FILE ALREADY HELD THE MISTAKE IN WRITING. slots_are_plausible() says "the
 * game's own empty is 0xFA, not 0", and treats an all-zero block as
 * uninitialised. That is right about what the port must not ADOPT and was
 * silently borrowed as a claim about what the port may WRITE, which does not
 * follow: 0xFA is the value the customize screen stores for a slot the player
 * emptied, and the absence of any choice is not that.
 *
 * SO AN UNCHOSEN SLOT IS LEFT ALONE. The port writes the slots the player
 * actually set and does not touch the rest, which is the difference between
 * applying a customization and imposing one. A player who genuinely wants a
 * character bare-headed still gets it: that is a chosen 0xFA arriving through
 * the customize screen, and the host policy filter can still force one, both of
 * which are choices this cannot distinguish from - see the note below. */
void write_effective() {
    uint16_t parts[kLsw2CustomSlots];
    effective_parts(parts);
    for (int i = 0; i < kLsw2CustomSlots; ++i) {
        /* SKIP A SLOT THAT HOLDS NO REAL CHOICE, so the game's own default for
         * it survives instead of being stamped over. For the five WEARABLES
         * that means the 0xFA "no part" sentinel. For the SIXTH, the weapon,
         * it means anything that is not a valid customize weapon (0x11..0x19):
         * the setter writes an out-of-range value RAW to the action row and so
         * a leftover like 40 would REPLACE the character's valid default weapon
         * with a row that has no asset, leaving it unarmed (measured
         * 2026-09-14). This is why the weapon needs its own predicate rather
         * than sharing the wearables' none-test. */
        const bool skip = (i < kWearableSlots) ? (parts[i] == kLsw2CustomNone)
                                               : !lsw2_is_custom_weapon(parts[i]);
        if (skip) {
            g_write_skipped[i] = true;
            continue;
        }
        g_write_skipped[i] = false;
        write_slot(i, parts[i]);
    }

    /* g_have_written gates the one-time initial push below. (The old adopt path
     * kept a g_last_written baseline here to tell its own writes from an
     * external edit; that path is gone - capture is the menu-commit override
     * now - so only the flag remains.) */
    g_have_written = true;

    /* AND WRITE THEM WHERE THE GAME ACTUALLY LOOKS, then ask it to look.
     *
     * The second observation of this: after a character switch the new
     * character still came up without the customizations.
     *
     * The customize slots above are what the CUSTOMIZE SCREEN edits, and the
     * game reads them when it dresses an actor - which has already happened by
     * the time a character id changes (the comment further down explains why
     * that race cannot be won). Holding the slots was the second attempt at
     * winning it and testing in play showed it did not work either.
     *
     * THE APPEARANCE RECORD IS NOT THE ANSWER EITHER. That route
     * depends on `0x020897F8`, which has exactly one caller and runs only when
     * `[r7+8] == 0x0F`. It is mode-gated and the lobby is not that mode, so the
     * record and its gate were written three times into something that never
     * ran. Kept because it IS correct for the mode that uses it and costs six
     * halfwords, but it is not what makes the switch work.
     *
     * WHAT MAKES THE SWITCH WORK IS THE TIMING, and it is handled in the tick
     * by watching the PENDING byte rather than the character id. See
     * pending_tick(). */
    const int station = lsw2_fork_session().local_station();
    if (station < 0 || station >= 4) return;

    const uint32_t rec =
        kLsw2AppearanceRecord + uint32_t(station) * kLsw2AppearanceStride;

    /* THE CHARACTER FIELD TOO, AND THIS IS WHY THE GATE NEVER FIRED.
     *
     * The third observation of this: customizations still did not
     * persistently apply between characters.
     *
     * `0x020897F8` only re-dresses a slot whose character is NOT 0x64, and
     * `lsw2_cheats_request_appearance()` refuses for the same reason. Nothing in
     * the port had ever written that field: peerparts.cpp deliberately does not
     * (the character rides the block header instead), and this function wrote
     * only the parts. So OFFLINE - where the game itself never populates the
     * record, because it exists for wireless play - the character stayed 0x64,
     * the gate was refused every time, and five correct halfwords sat in memory
     * being ignored. The mechanism was right and its precondition was missing.
     *
     * Writing it is safe here in a way it is not in peerparts: this is the LOCAL
     * station, and the value is the character the player demonstrably already
     * is - read from the actor, not chosen by the fork. It cannot fight the
     * roster rules because it only ever agrees with them. */
    const int who = lsw2_cheats_party_character(0);
    if (who >= 0 && who != int(kLsw2CharacterNone))
        bus_device_write16(0, rec, uint16_t(who));

    for (int i = 0; i < kLsw2CustomSlots && i < 5; ++i)
        bus_device_write16(0, rec + 2u + uint32_t(i) * 2u, parts[i]);

    if (!lsw2_cheats_request_appearance(station))
        std::fprintf(stderr,
                     "[fork] customization: slot %d still refuses (character "
                     "%d) - the record is not live\n", station, who);
}

}  // namespace

/* THE PENDING BYTE, WHICH IS THE ONLY SIGNAL EARLY ENOUGH.
 *
 * Three attempts tried to re-dress the actor after a character switch, and all
 * three were too late by construction. The game applies customization at BUILD
 * time: `0x02067838` reads a descriptor's kind byte, calls builder `table[kind]`
 * - index 0 being `0x02050C44` - and that builder reads the customize slots and
 * ends in the appearance apply. By the time `lsw2_cheats_party_character(0)`
 * changes, the actor already exists, so the build is over and the slots have
 * been read. This file's own comment said it: "the id is the announcement that
 * the moment has passed."
 *
 * `0x02091668` writes the requested character to `+0x92E` and its flag to
 * `+0x932`, one byte per slot, and the writer skips a slot whose flag is already
 * set - so a request SITS there until the game takes it up. **The build happens
 * while it sits.** Writing the player's parts when the request appears puts them
 * in the slots before the builder looks, which is what the previous three
 * attempts could never do.
 *
 * NOT A RE-APPLY. This does not correct the actor afterwards; it makes the
 * source correct beforehand, so the game dresses the new character properly the
 * first time. That is the difference between following the game's sequencing and
 * racing it. */
/* THE LEVEL ROOT, from the GENERATED levels.h as kLsw2LevelRootAddr - one
 * copy, and the right one: extract_level_table.py regenerates it from the
 * ROM, so a re-extraction updates every consumer instead of leaving
 * hand-written copies behind. */
/* The pending-character byte is lasthunter.h's kLsw2PendingCharacter. */

int g_last_pending = -1;

int pending_character(int slot) {
    const uint32_t addr = kLsw2LevelRootAddr + kLsw2PendingCharacter + uint32_t(slot);
    const uint32_t w = bus_device_read32(kArm9, addr & ~3u);
    return int(int8_t((w >> ((addr & 3u) * 8u)) & 0xFFu));
}

/* Returns true when a change was newly requested this frame. */
bool pending_tick() {
    const int now = pending_character(0);
    const bool fresh = (now >= 0 && g_last_pending < 0);
    g_last_pending = now;
    return fresh;
}

void lsw2_customization_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): writes into the game from one console's tick land at different steps. */
    if (lsw2_vwh_lockstep()) return;
    if (!ui_fork_build()) return;          /* the baseline is untouched */
    if (!lsw2_cheats_available()) return;  /* no context yet */

    /* THE MENU'S TWO OVERLAY EXTRAS, re-asserted on the live actor every frame.
     * Separate from the slot writes below: the moustache/sunglasses bits live on
     * the actor (not the customize staging block) and the game reads them per
     * frame, so this needs no build-timing dance - see apply_overlays(). Cheap
     * and idempotent; returns immediately when neither is chosen. */
    apply_overlays();

    /* PUSH THE LOADED SET INTO THE GAME ONCE. The port loads settings.json into
     * g_saved at startup; this puts it into the game so a customization saved
     * last session is worn without waiting for a character change. The
     * customization design requires the set to survive character switches and
     * every move between a level and an arena - a set that is loaded and never applied is not persistence. */
    if (g_have_saved && !g_have_written) {
        write_effective();
        std::fprintf(stderr,
                     "[fork] pushed the saved customization into the game "
                     "(%u %u %u %u %u weapon %u)\n",
                     unsigned(g_saved[0]), unsigned(g_saved[1]),
                     unsigned(g_saved[2]), unsigned(g_saved[3]),
                     unsigned(g_saved[4]), unsigned(g_saved[5]));
    }

    /* AHEAD OF THE BUILD, not after it - see pending_tick() above.
     *
     * A character change is REQUESTED before the game builds the new actor, and
     * the builder reads the customize slots while the request is still pending.
     * This is the one moment at which writing them is early enough, and it is
     * why the three previous attempts - all triggered by the character id
     * changing - could not work. */
    if (g_have_saved && pending_tick()) {
        write_effective();
        g_hold = kHoldFrames;
        g_blackout = kBlackoutFrames;
        std::fprintf(stderr,
                     "[fork] character change requested - parts written before "
                     "the build\n");
    }

    const int character = lsw2_cheats_party_character(0);
    if (character < 0) return;             /* no player actor this frame */

    if (g_last_character < 0) g_last_character = character;

    /* THE SWITCH. Arm the re-apply and stop believing the slots. */
    if (character != g_last_character) {
        g_last_character = character;
        if (g_have_saved) g_reapply_in = kReapplyDelay;
        g_blackout = kBlackoutFrames;
        return;
    }

    if (g_reapply_in > 0) {
        --g_reapply_in;
        if (g_reapply_in == 0)
            for (int i = 0; i < kLsw2CustomSlots; ++i) g_before[i] = read_slot(i);
        return;
    }

    if (g_reapply_in == 0) {
        write_effective();
        g_reapply_in = -1;   /* disarm, or the hold below never expires */

        /* AND KEEP WRITING THEM, for as long as the new character is still
         * being assembled.
         *
         * ONE WRITE WAS NOT ENOUGH and testing in play proved it: the log fired
         * and the character did not change. The single write was placed a frame
         * after the character id changes - but the id changes when the new
         * actor EXISTS, which is after the game has already dressed it. Trying
         * to be first by watching the id is not possible; the id is the
         * announcement that the moment has passed.
         *
         * Holding the slots instead does not need to win that race. Whenever
         * the game next reads them - this spawn or the next - it reads the
         * player's parts, because that is all that has been there since. If it
         * turns out the game never re-reads them, this costs five halfword
         * writes a frame for half a second and the diagnostic below says so. */
        g_hold = kHoldFrames;

        /* SAY WHAT WAS THERE, WHAT WE WROTE, AND WHAT STUCK.
         *
         * This line was seen printing while the character's
         * appearance did not change, and "re-applied" was a claim about the
         * write rather than about the result - the write is five halfword
         * stores and cannot fail visibly. Three things could be true and the
         * log distinguished none of them: the slots are not what dresses the
         * actor; the game overwrites them straight back; or they are right and
         * nothing re-reads them until the next spawn.
         *
         * Reading back immediately separates the second from the other two, and
         * printing the previous values says whether the game had already put
         * the new character's stock parts there. The original analysis of the
         * applier flagged this exact risk - that writing the slots and hoping the game re-applies was the
         * cheap option to try FIRST, not the one known to work. */
        if (g_apply_reports < kApplyReports) {
            ++g_apply_reports;
            /* "kept" is a slot the player never chose, which the port now
             * leaves holding whatever the game put there. */
            char wrote[5][8];
            for (int i = 0; i < 5; ++i) {
                if (g_write_skipped[i])
                    std::snprintf(wrote[i], sizeof wrote[i], "kept");
                else
                    std::snprintf(wrote[i], sizeof wrote[i], "%04X", g_saved[i]);
            }
            std::fprintf(stderr,
                         "[fork] customization write to character %d:\n"
                         "       was    %04X %04X %04X %04X %04X\n"
                         "       wrote  %4s %4s %4s %4s %4s\n"
                         "       reads  %04X %04X %04X %04X %04X\n",
                         character,
                         g_before[0], g_before[1], g_before[2], g_before[3],
                         g_before[4],
                         wrote[0], wrote[1], wrote[2], wrote[3], wrote[4],
                         read_slot(0), read_slot(1), read_slot(2), read_slot(3),
                         read_slot(4));
        }
        return;
    }

    /* THE HOLD. Re-assert the saved parts every frame of the window, so the
     * game reads them whenever it happens to look rather than only if it looks
     * on the one frame the port chose. */
    if (g_hold > 0) {
        --g_hold;
        write_effective();
        return;
    }

    if (g_blackout > 0) { --g_blackout; return; }

    /* THE CAPTURE USED TO LIVE HERE and it was the wrong model. It read the
     * staging block 0x02107890+0xA6..0xB0 (which RE 2026-09-18 showed is the
     * Character Creator subsystem, not the arena customize) and adopted
     * whatever it held - so a character's OWN stock parts, seen during a
     * switch, were captured and carried to the next character (Greedo's head,
     * IG-88's body/blaster, Boushh's helmet). The arena customize menu writes
     * the ACTOR, not this block, and only ever sets slot-3 headgear + a weapon
     * + two overlay bits. Capture now happens ONLY at the menu's own commit
     * (lsw2_customization_menu_commit, the FUN_0207AEF4 override) and keeps
     * ONLY the menu's whitelisted options - see below. This tick now only
     * APPLIES the saved set (through the writes above and gearapply.cpp). */
}

const uint16_t* lsw2_customization_saved() { return g_saved; }

namespace {

/* A CHARACTER'S STOCK HEADGEAR AND WEAPON, read exactly the way the customize
 * menu's own Reset row reads them (FUN_0207aef4 case 0, literal pool at
 * 0x0207B22C..0x0207B234):
 *   - character 0x4F: no headgear (0xFA) and weapon row 0x28 ("keep");
 *   - characters 0x49..0x4B (the creator's customs): six-byte records in RAM at
 *     0x02104DE2, headgear at +0, weapon at +5;
 *   - everyone else: headgear at 0x020EEBBB + c*5, weapon at 0x020EF581 +
 *     c*0x18 (the 0x18-byte character descriptor at 0x020EF57C, byte +5). */
struct Stock { uint16_t headgear; uint16_t weapon; };

Stock stock_of(uint8_t character) {
    if (character == 0x4Fu) return {kLsw2CustomNone, 0x28u};
    if (character >= 0x49u && character <= 0x4Bu) {
        const uint32_t rec = 0x02104DE2u + uint32_t(character - 0x49u) * 6u;
        return {read8(rec), read8(rec + 5u)};
    }
    return {read8(0x020EEBBBu + uint32_t(character) * 5u),
            read8(0x020EF581u + uint32_t(character) * 0x18u)};
}

}  // namespace

extern "C" void lsw2_arm9_FUN_0207aef4(void);

void lsw2_customization_menu_commit() {
    /* THE ARENA CUSTOMIZE MENU'S OWN COMMIT (0x0207AEF4), and the ONLY capture
     * path now. It applies the row the player just chose - a hat (case 2), a
     * weapon (case 1), moustache/sunglasses (cases 3/4), or Reset (case 0, which
     * re-dresses the character in its STOCK headgear + weapon and strips the
     * overlay bits). Run the game's commit first, then read the ACTOR it dressed
     * and record ONLY the menu's whitelisted options.
     *
     * This fires SOLELY on a menu action (0 direct callers - vtable-dispatched),
     * so unlike the old staging-poll and gear-applier paths it never observes the
     * game dressing a switched character in its own stock. And it needs no
     * special Reset hook: after Reset the actor shows the character's stock, which
     * is not in the whitelist, so the saved set clears to none. The menu never
     * sets slots 0/1/2/4, so those are forced empty and a character's stock
     * head/body/helmet can never be captured or carried forward. */
    /* READ THE OVERLAY BITS BEFORE THE GAME'S COMMIT, so the capture below can
     * tell a bit the player just TOGGLED from one the character already wore.
     * The customize preview actor keeps its identity across the commit. Fetched
     * here, not after, for exactly that before/after comparison. */
    const bool fork_capture = ui_fork_build() && !lsw2_cheats_in_arena_match();
    uint32_t actor = 0u;
    uint16_t before_extras = 0u;
    if (fork_capture) {
        const uint32_t a = lsw2_cheats_party_actor(0);
        if (a >= 0x02000000u && a < 0x02400000u) {
            actor = a;
            before_extras = read16(actor + 0x1A8u);
        }
    }

    g_cpu.R[15] = 0x0207AEF4u;
    lsw2_arm9_FUN_0207aef4();

    if (actor == 0u) return;   /* baseline, an arena match, or no live actor */

    const uint16_t headgear =
        read16(actor + kLsw2ActorSlotTable + uint32_t(kHeadgearSlot) * 2u);
    const uint16_t weapon = read8(actor + kLsw2ActorActionRow);

    /* AND NOT THE CHARACTER'S OWN STOCK, even when the menu lists it.
     *
     * The whitelists above assumed a stock item is never a menu option, so
     * that Reset - which re-dresses the character in its stock - would clear
     * the saved set on its own. It is an option for fifteen characters: eleven
     * carry a stock lightsaber the menu also offers (Vader's red 0x12 among
     * them) and four a stock hat 178. So Reset on Vader saved his red saber as
     * the player's weapon, and it followed to Ben Kenobi and every character
     * after. What the character already carries is
     * not a customization, however it got there. */
    const Stock stock = stock_of(read8(actor + kLsw2ActorCharacter));

    g_saved[0] = g_saved[1] = g_saved[2] = g_saved[4] = kLsw2CustomNone;
    g_saved[kHeadgearSlot] =
        (lsw2_is_customize_headgear(headgear) && headgear != stock.headgear)
            ? headgear : kLsw2CustomNone;
    g_saved[kWearableSlots] =
        (lsw2_is_custom_weapon(weapon) && weapon != stock.weapon)
            ? weapon : kLsw2CustomNone;

    /* THE TWO OVERLAY EXTRAS, CAPTURED BY TRANSITION - only what THIS commit
     * changed, never the character's stock. This is the fix for sunglasses
     * auto-applying without being chosen: the old capture read
     * the bit's absolute state after any commit, so a character whose stock look
     * sets 0x800 latched g_sunglasses and apply_overlays then forced it onto
     * everyone. Now a moustache/sunglasses row that flips its bit 0 -> 1 is the
     * player choosing that Extra, Reset (case 0) flips both 1 -> 0, and a bit set
     * before AND after (stock, or an untouched Extra during a hat/weapon commit)
     * is left exactly as it was - so stock overlays are never captured and a
     * genuine choice still persists across character switches via
     * apply_overlays. */
    const uint16_t after_extras = read16(actor + 0x1A8u);
    const uint16_t changed = uint16_t(before_extras ^ after_extras);
    if (changed & 0x400u) g_moustache  = (after_extras & 0x400u) != 0;
    if (changed & 0x800u) g_sunglasses = (after_extras & 0x800u) != 0;

    g_have_saved = true;
    g_dirty = true;
    std::fprintf(stderr,
                 "[fork] customize menu: saved headgear %u, weapon %u, "
                 "moustache %d, sunglasses %d (extras %04X -> %04X; slots "
                 "0/1/2/4 held empty)\n",
                 unsigned(g_saved[kHeadgearSlot]),
                 unsigned(g_saved[kWearableSlots]),
                 int(g_moustache), int(g_sunglasses),
                 unsigned(before_extras), unsigned(after_extras));
}

void lsw2_customization_observe_panel_apply(uint32_t /*part*/) {
    /* DELIBERATELY A NO-OP NOW. This fired from the gear-applier override for
     * every dispatched apply, which includes the game DRESSING a character in
     * its own stock parts - so it captured stock and carried it forward, the
     * leak that kept showing up in play. Capture moved to the customize menu's own
     * commit (lsw2_customization_menu_commit, the FUN_0207AEF4 override), which
     * fires ONLY on a menu action and records only the menu's whitelisted
     * options. The signature stays because headfix.cpp still calls it; keeping
     * the call site harmless is cheaper than threading a build flag through it. */
}

void lsw2_customization_load(const uint16_t* slots) {
    std::memcpy(g_saved, slots, sizeof g_saved);

    /* LOAD ONLY WHAT THE MENU CAN SET, which also HEALS a settings file that an
     * older build polluted. Earlier builds captured a character's stock parts
     * into slots 0/1/2/4 and out-of-set weapons/hats; those values are still in
     * people's settings.json (e.g. Greedo's head 16 in slot 0). The menu can
     * only ever set slot-3 headgear + a weapon, so on load everything else is
     * forced empty and slots 3/5 are kept only if they are genuine menu options.
     * A stale leaked value therefore drops out on the next save, with no manual
     * cleanup. */
    g_saved[0] = g_saved[1] = g_saved[2] = g_saved[4] = kLsw2CustomNone;
    if (!lsw2_is_customize_headgear(g_saved[kHeadgearSlot]))
        g_saved[kHeadgearSlot] = kLsw2CustomNone;
    if (!lsw2_is_custom_weapon(g_saved[kWearableSlots]))
        g_saved[kWearableSlots] = kLsw2CustomNone;

    g_have_saved = (g_saved[kHeadgearSlot] != kLsw2CustomNone ||
                    g_saved[kWearableSlots] != kLsw2CustomNone);
    g_dirty = false;
}

bool lsw2_customization_dirty() { return g_dirty; }

void lsw2_customization_mark_clean() { g_dirty = false; }

bool lsw2_customization_moustache()  { return g_moustache; }
bool lsw2_customization_sunglasses() { return g_sunglasses; }

void lsw2_customization_set_overlays(bool moustache, bool sunglasses) {
    /* The load path - the menu-commit capture (transition-based now) is what
     * else sets these. Does not mark the set dirty; lsw2_customization_load runs
     * just before and clears the flag. */
    g_moustache = moustache;
    g_sunglasses = sunglasses;
}
