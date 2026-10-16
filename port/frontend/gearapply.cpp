/* See gearapply.h. Dress the actor with the game's own applier. */

#include "gearapply.h"
#include "vwh.h"
#include "gearhop.h"

#include "cheats.h"
#include "customization.h"
#include "gearhop.h"
#include "guestcall.h"
#include "headfix.h"
#include "partslot.h"
#include "state.h"
#include "ui.h"

#include <cstdio>

extern "C" void lsw2_arm9_FUN_020063d4(void);

namespace {

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

/* The five part slots the applier understands. The sixth saved entry is the
 * weapon and goes through a different function - see below. */
constexpr int kParts = 5;
constexpr int kWeapon = 5;

/* The action row the weapon setter writes is cheats.h's
 * kLsw2ActorActionRow. */

uint32_t g_last_actor = 0;
/* Set once this actor has been dressed, cleared when the actor changes. See
 * the note at the top of the tick: this fires on a new actor, not per frame. */
bool g_settled = false;
int g_reports = 0;
constexpr int kReports = 6;

/* A CALL THAT DID NOT RETURN MUST NOT BE MADE AGAIN.
 *
 * Applying customizations once froze the game. The log of that run has this
 * line 4,344 times in 4,334 frames:
 *
 *     [fork] gear applier DID NOT RETURN for slot 0 part 0 -
 *            not calling it again this frame
 *
 * "this frame" was the whole of the guard. Every frame it tried the same call
 * with the same arguments, the call ran away into the guest and unwound, and
 * the frame budget went with it - 1,629 audio underruns in that run. THAT is
 * the freeze: not a deadlock, a call that costs a frame
 * and is repeated forever.
 *
 * So a failure is remembered. The pair is the ARGUMENTS, not just the slot: a
 * different part in the same slot is a different call and deserves its own
 * attempt, and the player changing their mind must not be permanently punished
 * by one bad id. Cleared when the actor changes, because a new actor is a new
 * question.
 *
 * NOT A COVER FOR A BAD PART ID - those are fixed at the source below and in
 * customization.cpp. This is the backstop that stops the NEXT bad id being a
 * freeze instead of a log line, which is the difference between a bug and an
 * unplayable build. */
constexpr int kSlotCount = 6;
uint16_t g_failed_part[kSlotCount] = {0, 0, 0, 0, 0, 0};
bool g_failed[kSlotCount] = {false, false, false, false, false, false};

void forget_failures() {
    for (int i = 0; i < kSlotCount; ++i) g_failed[i] = false;
}

bool already_failed(int slot, uint16_t part) {
    return slot >= 0 && slot < kSlotCount && g_failed[slot] &&
           g_failed_part[slot] == part;
}

void remember_failure(int slot, uint16_t part) {
    if (slot < 0 || slot >= kSlotCount) return;
    g_failed[slot] = true;
    g_failed_part[slot] = part;
}

/* PART 0 IS NOT A PART. The game's "this slot is empty" is 0xFA - the two apply
 * loops at 0x02006220 skip a slot holding it - and
 * customization_policy.cpp's filter already says in as many words that writing
 * a zero would make "the guest treat the slot as empty instead of as part id
 * 0". A zero reaching here means something upstream stored one, and asking the
 * applier to fit part number zero to a head is what ran away. */
bool is_a_part(uint16_t part) {
    return part != 0u && part != kLsw2CustomNone;
}

uint8_t read8(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

uint16_t read16(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint16_t((addr & 2u) ? (w >> 16) : w);
}

}  // namespace

void lsw2_gearapply_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): re-dressing an actor on one console is a divergence. */
    if (lsw2_vwh_lockstep()) return;
    if (!ui_fork_build()) return;

    const uint32_t actor = lsw2_cheats_party_actor(0);
    if (actor < kRamLow || actor >= kRamHigh) return;

    uint16_t want[kLsw2CustomSlots] = {0};
    lsw2_customization_effective(want);

    /* A CHARACTER SWITCH IS JUST A NEW ACTOR, and worth saying once so the
     * log can tell "it re-dressed on switch" from "it never ran". */
    const bool switched = (actor != g_last_actor);
    g_last_actor = actor;
    /* A NEW ACTOR IS A NEW QUESTION, so a part that could not be fitted to the
     * old one gets one attempt on the new one. */
    if (switched) forget_failures();

    /* ONLY WHEN THE ACTOR IS NEW.
     *
     * Driving the customize machinery to re-apply hats is the wrong model; the
     * right one is to load the state of the hats. The freeze described above
     * is what it cost to get that wrong. This ran on EVERY frame, comparing the actor's slots against
     * the wanted set and calling the game's dresser whenever they differed - so
     * a part the dresser would not accept was retried sixty times a second
     * forever, and the frame budget went with it.
     *
     * The state is what carries the customization. customization.cpp writes the
     * player's set into the game's own customize slots, which is where the game
     * READS a character's parts when it builds one - that is the load, and it
     * is the mechanism the cartridge itself uses.
     *
     * This call is not a substitute for that; it is for the one case the slots
     * cannot cover, an actor that ALREADY EXISTS when the set changes. So it
     * fires when the actor is new and then stops, instead of standing over the
     * game re-dressing it. g_settled is cleared on a switch, which is exactly
     * the moment the customizations must auto-apply to the changed character:
     * immediately upon the switch, a frame after it. */
    if (switched) g_settled = false;
    if (g_settled) return;

    int applied = 0;
    for (int slot = 0; slot < kParts; ++slot) {
        const uint16_t part = want[slot];
        /* THE SENTINEL IS "LEAVE IT ALONE", not "clear it". 0x02006220 writes
         * 0xFA into the slot when the STOCK table says a character has no part
         * there, but that is the game deciding a character's own appearance.
         * Here the sentinel means the host disallowed this slot, and the right
         * result is the character the game already built - so we skip. */
        if (!is_a_part(part)) continue;

        const uint32_t addr = lsw2_actor_slot_addr(actor, slot);
        if (read16(addr) == part) continue;

        /* SAID ONCE, THEN SILENT. See g_failed above: this used to retry every
         * frame forever, which is the reported freeze. */
        if (already_failed(slot, part)) continue;

        /* NOT READY IS NOT FAILED - measured 2026-09-01. Every lifted function
         * refuses before its first instruction while the guest is HALTED, and
         * the fork tick runs between frames where the guest usually is. Without
         * this the first unlucky frame recorded a PERMANENT failure below and
         * the part was never applied again for the life of the actor, which is
         * a required feature silently switching itself off. Deferring costs one
         * frame; remembering costs the run. See guestcall.h. */
        if (!lsw2_guest_ready()) return;

        /* QUEUED FOR THE GUEST'S OWN THREAD, not called from here.
         *
         * `lsw2_guest_call` cannot run this function at all: it was
         * measured reaching an idle wait with the machine's event queue
         * EMPTY, so the interrupt it waits for can never be raised between
         * frames. The hop in gearhop.cpp runs it where the queue is live. */
        if (!lsw2_gearhop_request(actor, part, slot)) continue;
        ++applied;

        /* THE HEAD FIX'S CORRECTION MOVED TO THE HOP'S COMPLETION, which is where it
         * belongs: it must run AFTER the applier has actually run, and until
         * the hop existed the applier never completed at all - so calling it
         * here was a correction applied to a dressing that had not happened.
         * gearhop.cpp calls it when the guest returns. */

        if (g_reports < kReports) {
            ++g_reports;
            std::fprintf(stderr,
                         "[fork] dressed actor %08X slot %d: part %u (now %u)"
                         "%s\n",
                         unsigned(actor), slot, unsigned(part),
                         unsigned(read16(addr)),
                         switched ? "  [after a character switch]" : "");
        }
    }

    /* THE WEAPON, through its own setter. It is a BYTE at actor + 0x6C3 and
     * not one of the five halfword slots, which is why it cannot ride the loop
     * above - reading it as a halfword would compare against the wrong thing. */
    /* THE SAME TWO GUARDS AS THE PART LOOP, and it needs them for the same
     * reason: this is a guest call in a per-frame tick. It had neither, so a
     * weapon the setter cannot fit would have been a second freeze the moment
     * the part loop stopped being one. */
    /* ONLY A VALID CUSTOMIZE WEAPON, not merely "not the sentinel". is_a_part
     * accepts any non-zero, non-0xFA value, but the weapon setter only maps
     * rows 0x11..0x19; anything else is written RAW to the action row and
     * OVERWRITES the character's valid default with a row that has no weapon
     * asset - the "spawns unarmed" bug, from a leftover 40 (measured
     * 2026-09-14). So an out-of-range weapon is left alone: the game's own
     * default weapon stays, and the hunter is armed. */
    const uint16_t weapon = want[kWeapon];
    if (lsw2_is_custom_weapon(weapon) &&
        read8(actor + kLsw2ActorActionRow) != uint8_t(weapon) &&
        !already_failed(kWeapon, weapon)) {
        /* THROUGH THE GEAR HOP, NOT THE HOST-SIDE SEAM. Measured 2026-09-12 in
         * the first lobby-return run under a correct admission gate:
         * on the HOST, entering the arena (state 41) re-dresses the character,
         * this call unwound - "02004404 did not return ... active_cpu=ARM7" -
         * and the very next WH DATASHARING step reported WM_ERRCODE_ILLEGAL_STATE
         * (5), as did every step after it, 2,991 times, until the return crashed
         * the ARM7 (64 unmapped reads from pc=02380068). The CHILD entered 41 at
         * the same instant with no unwind and no storm, and returned cleanly.
         * At boot the same unwind is harmless because no session is running;
         * inside the WM parent's data-sharing cadence it is not.
         *
         * The general rule was already established: a host guest call
         * reaches an idle wait between frames and can never complete, and the
         * hop - which runs the function on the guest's own thread where the
         * queue is live - is the answer. The applier and the party refresh moved
         * across then; this was the one customization call left on the old
         * seam.
         *
         * "queued" is what success means now: the call completes on a later
         * frame, and `applied` counts the request, as it does for the applier. */
        if (lsw2_gearhop_request_call(kLsw2WeaponSetter, actor, weapon, 0u,
                                      kLsw2WeaponHopTag)) {
            ++applied;
            if (g_reports < kReports) {
                ++g_reports;
                std::fprintf(stderr,
                             "[fork] arming actor %08X with weapon %u through "
                             "the gear hop (carries %u now)\n",
                             unsigned(actor), unsigned(weapon),
                             unsigned(read8(actor + kLsw2ActorActionRow)));
            }
        } else {
            remember_failure(kWeapon, weapon);
            std::fprintf(stderr,
                         "[fork] weapon setter: the hop queue REFUSED %u - not "
                         "trying it on this actor again\n", unsigned(weapon));
        }
    }

    /* DONE WITH THIS ACTOR until it changes. Reached whether or not anything
     * was applied: "nothing to do" is as settled as "everything applied", and
     * re-deciding that every frame is the per-frame re-dressing described
     * above. */
    g_settled = true;
    (void)applied;
}
