/* See lobbyrespawn.h. Re-enter the lobby in the mode that spawns. */

#include "lobbyrespawn.h"
#include "vwh.h"

#include "cheats.h"
#include "levels.h"
#include "netcount.h"
#include "guestcall.h"
#include "statelog.h"
#include "whsession.h"
#include "forksession.h"
#include "lasthunter.h"
#include "ui.h"
#include "state.h"

#include <cstdio>
#include <cstdlib>

extern "C" void lsw2_arm9_FUN_02088b84(void);
extern "C" void lsw2_arm9_FUN_02089e60(void);

namespace {

constexpr int kArm9 = 9;

/* The request record, the built object's fields and the status values all come
 * from statelog.h. They used to live in cheats.cpp's anonymous namespace; the
 * moment this file needed them the `dup_constants` gate caught the copy, which
 * is the gate working, so they were promoted rather than duplicated. */

uint32_t read32(uint32_t addr) { return bus_device_read32(kArm9, addr); }
void write32(uint32_t addr, uint32_t v) { bus_device_write32(kArm9, addr, v); }

bool ram_pointer(uint32_t v) { return v >= 0x02000000u && v < 0x02400000u; }

uint8_t read8(uint32_t addr) {
    const uint32_t w = read32(addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

}  // namespace

bool lsw2_lobby_respawn_armed() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("LSW2_LOBBY_RESPAWN");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached == 1;
}

namespace {

/* WHAT IS PENDING, AND FOR HOW LONG. `why` is the caller's name, kept for the
 * log; the countdown bounds how many ticks we keep trying before giving up, so
 * a request that can never be satisfied says so once instead of retrying for
 * the rest of the run. */
const char* g_pending_why = nullptr;
int g_pending_ticks = 0;

/* THE VERIFY STAGE, and why the position cannot be read straight after.
 *
 * `lsw2_cheats_warp` REQUESTS a level; the game's own pump builds it over the
 * frames that follow. Reading the player's position on the next instruction
 * would report "unchanged" every single time, no matter how well the reload
 * worked - which is the same clean-no-op-looks-like-success trap the
 * spawn-dispatcher measurement fell into, one layer along. So the before-position is kept and compared
 * once the reload has had time to happen. */
const char* g_verify_why = nullptr;
int g_verify_ticks = 0;
uint32_t g_verify_actor = 0;
int32_t g_verify_pos[3] = {0, 0, 0};
constexpr int kVerifyDelayTicks = 600;   /* ~10s - a level load, generously */

/* THE ONE READ THE SAME-LEVEL CRASH LEFT OPEN, sampled rather than snapshotted.
 *
 * The per-frame free walk that kills a same-level re-entry is guarded by the
 * LEVEL-STATE HOLDER `[0x0210930C+0x68]` and `[holder+0x384]`: if either is
 * null it returns and does nothing. A proper level teardown nulls the holder,
 * which is why a CROSS-level warp survives - the walk is switched off while the
 * list is being rebuilt.
 *
 * The hypothesis is that a SAME-level request never nulls it, so the walk keeps
 * running over a list the rebuild is moving underneath it. Two snapshots cannot
 * test that: the null is TRANSIENT, lasting only as long as the teardown, and
 * either snapshot could easily miss it. So this samples every tick across the
 * whole transition and records whether it was EVER null - the absence of a null
 * across ~600 consecutive samples is a much stronger statement than its absence
 * at two arbitrary moments.
 *
 * Before reading an absence as evidence, ask whether the instrument could see
 * it. This one is built so it could. */
/* THE LEVEL THE `pair` MODE STILL OWES, or -1. Served by the tick once the
 * manager is idle, which is also once the state request's teardown has run -
 * and that teardown is the whole reason this mode exists. */
int g_owed_level = -1;

bool g_holder_seen_null = false;
int g_holder_null_first_tick = -1;
int g_holder_samples = 0;
/* ~30 seconds at 60fps. It was 240 (~4s), chosen before it was known that the
 * PLAYER'S ACTOR is one of the things being waited for - and the actor is built
 * by the level, not by the menu, so four seconds after Start Lobby was not
 * enough. A budget exists to stop an impossible request retrying forever, not
 * to express an expectation about how long a possible one takes. */
constexpr int kPendingTickBudget = 1800;

bool do_respawn(const char* why);

}  // namespace

void lsw2_lobby_respawn_arm(const char* why) {
    if (!lsw2_lobby_respawn_armed()) return;
    g_pending_why = why;
    g_pending_ticks = kPendingTickBudget;
    std::fprintf(stderr, "[lobbyrespawn] %s: armed, waiting for a settled guest\n",
                 why ? why : "?");
}

void lsw2_lobby_respawn_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): a respawn requested from one console is a divergence. */
    if (lsw2_vwh_lockstep()) return;
    /* SERVE AN OWED LEVEL FIRST. The state request has torn the level down; the
     * rebuild is what puts a player back, and it must be a SEPARATE tick
     * because the two requests share one block. Retried until the manager
     * accepts, so this does not have to land on a particular frame. */
    if (g_owed_level >= 0 && lsw2_cheats_manager_idle()) {
        const int want = g_owed_level;
        if (lsw2_cheats_warp_cold(want)) {
            g_owed_level = -1;
            std::fprintf(stderr,
                         "[lobbyrespawn] owed level %d asked for, AFTER the "
                         "state request's teardown - this is the pairing "
                         "the same-level crash was missing\n",
                         want);
        }
    }

    /* THE DEFERRED VERIFY, first, because it is the half that reports a result.
     *
     * The actor is re-read from the array rather than reused: a level reload
     * REBUILDS the player, so the address the request was issued against may not
     * be the address that exists afterwards - which has been seen to happen
     * inside a single arena match when somebody died. Comparing a stale pointer's memory
     * would be reading a dead object and calling it a position. */
    /* SAMPLE THE GUARD EVERY TICK of the transition - see the note above on why
     * two snapshots would not do. `[holder+0x384]` is read only when the holder
     * itself is a RAM pointer, because dereferencing a torn-down holder is the
     * very fault being investigated. */
    if (g_verify_why) {
        ++g_holder_samples;
        const uint32_t holder = read32(kLsw2LevelRootAddr + kLsw2LevelObjOffset);
        const bool off = !ram_pointer(holder) ||
                         read32(holder + kLsw2LevelHolderGuard) == 0u;
        /* REPORT THE MOMENT IT HAPPENS, not at the end of the countdown.
         *
         * The first version only printed its summary at the verify, 600 ticks
         * later - and the same-level run kills the console well before that, so
         * it reported nothing at all about the very crash it was built to
         * measure. An instrument that only speaks after the patient survives
         * cannot describe the deaths. */
        if (off && !g_holder_seen_null) {
            g_holder_seen_null = true;
            g_holder_null_first_tick = g_holder_samples;
            std::fprintf(stderr,
                         "[lobbyrespawn] %s: GUARD went OFF at tick %d - the "
                         "level WAS torn down, so the per-frame free walk is "
                         "switched off while the list is rebuilt\n",
                         g_verify_why, g_holder_samples);
        }
    }

    if (g_verify_why && --g_verify_ticks <= 0) {
        const char* why = g_verify_why;
        g_verify_why = nullptr;

        /* THE DISCRIMINATOR, reported before the position, because it is the
         * thing this run exists to measure. */
        std::fprintf(stderr,
                     "[lobbyrespawn] %s: GUARD - the per-frame free walk's gate "
                     "([%08X+%02X] and [holder+%03X]) was OFF at some point: %s"
                     "  (%d ticks sampled, first OFF at tick %d)\n",
                     why, unsigned(kLsw2LevelRootAddr),
                     unsigned(kLsw2LevelObjOffset), unsigned(kLsw2LevelHolderGuard),
                     g_holder_seen_null
                         ? "YES - the level WAS torn down, the walk was switched off"
                         : "NO - the walk never stopped, so the rebuild moved the "
                           "list underneath it",
                     g_holder_samples, g_holder_null_first_tick);
        g_holder_seen_null = false;
        g_holder_null_first_tick = -1;
        g_holder_samples = 0;

        const unsigned s = read8(kLsw2NetCountObject + kLsw2NetLocalStation);
        const uint32_t now_actor = s < 4u
            ? read32(kLsw2CameraActorArray + s * 4u) : 0u;
        if (!ram_pointer(now_actor)) {
            std::fprintf(stderr,
                         "[lobbyrespawn] %s: VERIFY - slot %u has no actor now; "
                         "the reload did not leave a player behind\n", why, s);
        } else {
            const int32_t ax = int32_t(read32(now_actor + kLsw2ActorPos + 0u));
            const int32_t ay = int32_t(read32(now_actor + kLsw2ActorPos + 4u));
            const int32_t az = int32_t(read32(now_actor + kLsw2ActorPos + 8u));
            const bool moved = ax != g_verify_pos[0] || ay != g_verify_pos[1] ||
                               az != g_verify_pos[2];
            std::fprintf(stderr,
                         "[lobbyrespawn] %s: VERIFY - actor %08X%s position "
                         "(%d,%d,%d) -> (%d,%d,%d)  RESPAWNED=%s\n",
                         why, unsigned(now_actor),
                         now_actor != g_verify_actor ? " (REBUILT - a new actor)" : "",
                         g_verify_pos[0], g_verify_pos[1], g_verify_pos[2],
                         ax, ay, az,
                         moved ? "yes - the player was moved"
                               : "NO - the reload did not move the player");
        }
    }

    if (!g_pending_why) return;
    if (--g_pending_ticks <= 0) {
        std::fprintf(stderr,
                     "[lobbyrespawn] %s: GAVE UP after %d ticks - the guest was "
                     "never both callable and settled\n",
                     g_pending_why, kPendingTickBudget);
        g_pending_why = nullptr;
        return;
    }
    /* Not ready yet is the ordinary case and says nothing; try again next tick. */
    if (!lsw2_guest_ready()) return;

    /* AND THE STATE MACHINE MUST BE SETTLED TOO, which is a SEPARATE condition
     * from the guest being callable and cost this file a run to learn.
     *
     * The first arm/tick run reached a callable guest and found the manager's
     * status at 02 - a transition in flight, right after Start Lobby binds the
     * transport. The old code treated that as its one attempt and gave up, even
     * though the message it printed says in as many words that it is NOT a
     * failure of the request. Waiting is the correct response to "not yet"; the
     * tick budget above is what stops it waiting forever. */
    if ((read32(kLsw2StateManager + kLsw2SlotStatus) & 0xFFu) != kLsw2StatusBuilt)
        return;

    /* AND THE PLAYER MUST EXIST, which is a THIRD condition and was measured the
     * same way the other two were - by a run that came back clean and useless.
     *
     * The first spawn-dispatcher run called `FUN_02089e60` with no fault and no
     * unmapped access, and reported "slot 0 has no actor in the array". Probing
     * the console ninety seconds later showed `0x02109358` holding `0211FFA0` -
     * the actor was simply not built yet when the tick fired. Asking a spawn
     * dispatcher to place a player that does not exist is a clean no-op, and a
     * clean no-op reads exactly like a fix that works.
     *
     * So wait for the actor too. Bounded by the same tick budget. */
    {
        const unsigned s = read8(kLsw2NetCountObject + kLsw2NetLocalStation);
        if (s >= 4u) return;
        if (!ram_pointer(read32(kLsw2CameraActorArray + s * 4u))) return;
    }

    const char* why = g_pending_why;
    /* Clear BEFORE acting: whatever happens, this is attempted once. A retry
     * loop around a request that reloads a level is how the earlier attempts
     * turned one bad idea into a hung console. */
    g_pending_why = nullptr;
    do_respawn(why);
}

void lsw2_lobby_seat_peers_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): the game's own session seats peers; a per-console swap desyncs the lobby. */
    if (lsw2_vwh_lockstep()) return;
    /* THE REMOTE-AVATAR SEAT, via the game's own per-player character poller.
     * The lobby spawns slot 1..3 as the single-player companion; the
     * KIND-0 builder that would build them as the peer's hunter runs once, before
     * any peer, so it never sees the peer. Rather than re-run the builder (a
     * level reload double-frees same-level) or the spawn dispatcher (a
     * guest-call to FUN_02089e60 halts the ARM9 on a GX IRQ, measured), request
     * the peer's character for each seated slot the SAME way the local player's
     * hunter is applied: write the pending-character byte and let the game's own
     * poller (0x02091604) swap that slot's companion to the peer's hunter, on the
     * guest thread in the ordinary frame. No guest-call, no reload, no wedge - the
     * exact mechanism lsw2_request_character already uses for slot 0. A brief
     * companion->hunter switch when a peer connects is fine for a remote avatar
     * (it is why the local player uses the builder instead). Idempotent:
     * lsw2_request_character_for no-ops once the slot already is that character. */
    if (!ui_fork_build()) return;
    if (lsw2_cheats_current_level() != kLsw2ArenaLobbyLevel) return;

    Lsw2ForkSession& session = lsw2_fork_session();
    if (session.state() == Lsw2SessionState::Offline) return;

    for (int slot = 1; slot < int(kLsw2PartySlots); ++slot) {
        const int chr = session.peer_slot_character(slot);
        if (chr < 0 || chr >= 0x54) continue;   /* no seated peer / not on wire yet */
        if (lsw2_request_character_for(slot, chr)) {
            std::fprintf(stderr,
                         "[fork] seat peers: requested slot %d as the connected "
                         "peer's hunter id %d (game poller swaps the companion)\n",
                         slot, chr);
        }
    }
}

namespace {

bool do_respawn(const char* why) {
    if (!lsw2_lobby_respawn_armed()) return false;

    /* ONLY FROM A SETTLED STATE. cheats.cpp paid for this one: requesting a
     * level while one is still building tears down a live object and the guest
     * carries on executing into instruction words as if they were data. The
     * pump leaves the status byte at 3 when it is finished. */
    const uint32_t status = read32(kLsw2StateManager + kLsw2SlotStatus);
    if ((status & 0xFFu) != kLsw2StatusBuilt) {
        std::fprintf(stderr,
                     "[lobbyrespawn] %s: refused - status %02X, a transition is "
                     "in flight (this is NOT a failure of the request)\n",
                     why, unsigned(status & 0xFFu));
        return false;
    }

    const uint32_t state = read32(kLsw2StateManager + kLsw2SlotResult);
    if (!ram_pointer(state)) {
        std::fprintf(stderr, "[lobbyrespawn] %s: refused - state %08X is not RAM\n",
                     why, unsigned(state));
        return false;
    }
    const uint32_t kind = read32(state + kLsw2StateKind);
    if (kind != kLsw2LevelStateKind) {
        std::fprintf(stderr,
                     "[lobbyrespawn] %s: refused - the built state's kind is %u, "
                     "not a level\n", why, unsigned(kind));
        return false;
    }

    const uint32_t was_mode = read32(state + kLsw2StateMode);
    const uint32_t was_level = read32(state + kLsw2StateLevel);
    const uint32_t arg_c = read32(state + kLsw2StateArgC);
    /* THE LEVEL COMES FROM THE BUILT STATE OBJECT ITSELF, `[state+0x0C]`, which
     * is `was_level` above.
     *
     * The cartridge reads it with `FUN_020917b4` ([[0x0210930C+0x68]+0x479]) and
     * cheats.h's `_via_state` walks that same chain, so that was the obvious
     * choice - and on the tick where everything else is finally ready it
     * returned -1 and refused the whole request. cheats.h says why: that chain
     * is written by the game's ORDINARY entry path, and the fork does not always
     * arrive that way.
     *
     * `[state+0x0C]` is the same fact on an object this function has already
     * proved is in RAM and is a level state, so it cannot be unavailable here by
     * construction. The `_via_state` reading is still logged beside it, because
     * a disagreement between the two is worth seeing rather than hiding. */
    const int level_via_state = lsw2_cheats_current_level_via_state();

    /* THE CONTROL, and this experiment is useless without it.
     *
     * The same-level warp corrupted the guest: 27 unmapped accesses inside
     * `FUN_02055210`, a linked-list FREE loop, dereferencing words that are ARM
     * instructions (`E59D0074` is `LDR r0,[r13,#0x74]`). That is a list being
     * walked after it was already torn down.
     *
     * But `lsw2_cheats_warp` is a SHIPPED feature that works, so the defect
     * cannot simply be "the warp". The distinguishing question is whether
     * re-entering the level the player is ALREADY ON is what breaks it, and the
     * only way to know is to warp somewhere else from the identical position in
     * the identical run shape.
     *
     * `LSW2_LOBBY_RESPAWN_LEVEL` overrides the destination for exactly that.
     * Unset, it re-enters the current level, which is what the lobby respawn
     * wants. */
    uint32_t level = was_level;
    if (const char* v = std::getenv("LSW2_LOBBY_RESPAWN_LEVEL")) {
        if (*v) level = uint32_t(std::atoi(v));
    }

    /* THE SPAWN, WITHOUT RELOADING THE LEVEL.
     *
     * The level re-entry was rejected on three grounds, and the third is
     * the one that points here: the reload was never the respawn. The reason
     * mode 0x0E mattered at all is that it is one of the modes reaching
     * `FUN_02089e60`, the slot 0..3 spawn dispatcher, with the player container
     * `0x02104DF4`:
     *
     *     FUN_0208c888 mode 0x0E / 0x12  ->  FUN_02089c88  ->  FUN_02089e60
     *     FUN_0208c888 mode 0x04         ->                    FUN_02089e60
     *
     * The level reload is the vehicle the game happens to use to get there
     * while it is already tearing a session down. Asking for the destination
     * directly skips the part that broke the console twice, and it cannot end
     * the session, because it has nothing to do with the session.
     *
     * AND THE QUESTION THAT HAD TO BE ASKED FIRST WAS ASKED FIRST. What made
     * the previous attempt unfixable was a polling call reached indirectly, so
     * `tools/sweep_guestcall_safety.py` now exists to answer that before the
     * code is written rather than after. `FUN_02089e60` comes back with **no
     * return-value spin through depth 4, 105 functions reached** - against a
     * detector that finds the real WH_End poll unaided, and a baseline of the
     * eight functions this port already host-calls successfully. That is
     * better evidence than any host call here has had. It is not a guarantee,
     * which is why this is still behind LSW2_LOBBY_RESPAWN.
     *
     * AND IT IS A NO-OP HERE. MEASURED. The call is SAFE - zero
     * unmapped accesses, zero faults, console healthy afterwards, which is the
     * first thing in this whole effort that did not break something. It also does
     * nothing, and the reason is in the container rather than in the call:
     *
     *     [container+0x290A]           = -1   -> FUN_02089e60 takes its SHORT
     *                                           path (two FUN_0208a31c calls
     *                                           with a -1 spawn index) and returns
     *     party record slot 0, char    = 100  = kLsw2CharacterNone
     *
     * The lobby's party records say there is nobody to spawn and no spawn point
     * to spawn them at. The multiplayer container is simply not in use on an
     * ordinary level - which is consistent with the walkaround enable coming
     * from the zone route instead.
     *
     * That also explains the earlier mode-0x0E crashes after the fact: re-entering the lobby in the CO-OP mode ran a spawn pass over
     * these same empty records. The garbage pointers were not a missing
     * preparation step; they were a co-op pass with no co-op data.
     *
     * So this is left in place as an ARMED DIAGNOSTIC, not as a fix. It reports
     * the two container fields above so the next run says why nothing moved. */
    /* WHERE THE PLAYER IS, BEFORE AND AFTER. "The call returned and nothing
     * crashed" is not the claim the lobby respawn needs - the claim is that the player was
     * RESPAWNED, and a position that does not move says the call did nothing
     * just as loudly as a crash would. This project has read "it ran" as "it
     * worked" before.
     *
     * The actor is read from the live array rather than hardcoded, because
     * actor addresses differ per level and change even within a match when
     * somebody dies. */
    const unsigned slot = read8(kLsw2NetCountObject + kLsw2NetLocalStation);
    const uint32_t actor = slot < 4u
        ? read32(kLsw2CameraActorArray + slot * 4u) : 0u;
    int32_t bx = 0, by = 0, bz = 0;
    if (ram_pointer(actor)) {
        bx = int32_t(read32(actor + kLsw2ActorPos + 0u));
        by = int32_t(read32(actor + kLsw2ActorPos + 4u));
        bz = int32_t(read32(actor + kLsw2ActorPos + 8u));
    }

    /* THE TWO FIELDS THAT DECIDE WHETHER THE CALL CAN DO ANYTHING. Reported
     * every time, because "it ran and nothing moved" has to name which gate
     * closed - and both of these were read only after a run had already been
     * scored as a clean success. */
    const int8_t spawn_index = int8_t(read8(kLsw2PlayerContainer + kLsw2ContainerSpawn));
    const uint32_t party_char = read32(kLsw2PlayerContainer + kLsw2ContainerParty) & 0xFFFFu;

    /* WHAT A CO-OP START ACTUALLY DOES: IT RELOADS THE LEVEL.
     *
     * The spawn-dispatcher measurement closed the container route - the lobby
     * has no spawn point and no party characters, so no spawn dispatcher can
     * place anybody there - and it left one reading open. The goal is to
     * respawn the way starting a co-op game does, and a co-op start does not
     * spawn anyone: it re-requests the level,
     * and the LEVEL'S OWN ENTRY places the player. That is also how the game
     * places the player every other time it loads the cantina.
     *
     * If that is right the mechanism is the reload after all - but in the
     * level's OWN mode, `Ordinary` (0x05), not the co-op mode 0x0E that the
     * earlier attempts tried. `lsw2_cheats_warp` is exactly that request and it is
     * already proven code: it preserves the live state's mode, which in the
     * lobby IS 0x05, and it carries cheats.cpp's own settled-state guard.
     *
     * So this is not a fifth guess at a new mechanism. It is the port's existing
     * warp, aimed at the level the player is already on, measured with the
     * position logging added for the spawn-dispatcher run - which is the thing
     * the three earlier attempts at this never had. */
    /* WHICH ACTION, because the container route is REOPENED.
     *
     * The spawn-dispatcher run called the dispatcher and it did nothing, and closed that
     * route. Both of the readings it closed it on were taken on a SOLO console,
     * and the container it read is the MULTIPLAYER container: with a peer
     * admitted the spawn index reads 16 rather than -1 and slot 0 carries a real
     * character. So `FUN_02089e60` was called against an empty container, which
     * is why it was a clean no-op - not because the route is wrong.
     *
     * Both actions are kept and selectable so they can be A/B'd in the same run
     * shape, which is the only way anything here has ever been settled:
     *
     *     LSW2_LOBBY_RESPAWN_MODE=spawn   FUN_02089e60(container) - the default
     *                                     now, and the reopened route
     *     LSW2_LOBBY_RESPAWN_MODE=warp    lsw2_cheats_warp - measured:
     *                                     works CROSS-level, kills the
     *                                     console on the SAME level
     *
     * The default is `spawn` because warping to the level you are already on is
     * measured to kill the guest, and a default that does that is a trap for
     * whoever arms this next. */
    /* A THIRD MODE: `state` - ASK FOR THE STATE, TOUCH NOTHING ELSE.
     *
     * WHY IT IS WORTH A RUN, when the other two are measured failures. The
     * `spawn` mode calls FUN_02089e60, which was shown to take its short
     * path when the container is empty - and the lobby respawn's actual case is
     * the host pressing Start Lobby ALONE, so the container IS empty exactly
     * then. The `warp` mode asks for the level the host is already on,
     * which was measured as a double free in FUN_02055210.
     *
     * Both of those exhaust what the port could do before the state-request
     * writer existed. It can now do a third thing:
     * `lsw2_wh_write_state_request` writes the state request
     * block the way the cartridge writes it to itself, needing no guest call
     * and no dispatch. Asking for state 48 - the cartridge's own
     * default arm, what an ordinary level entry produces - rebuilds
     * the STATE without asking for the level, which is the one combination
     * neither previous mode could express.
     *
     * WHETHER THAT RESPAWNS ANYONE IS EXACTLY THE QUESTION and it is not being
     * assumed. The deferred verify above is what answers it: the position is
     * read before, and again kVerifyDelayTicks later from a re-read actor. If
     * the player does not move, this mode joins the other two as measured and
     * closed, and the honest answer for the lobby respawn becomes that the port
     * cannot do it without a guest-thread capability it does not have. */
    /* AND A FOURTH: `pair` - THE STATE REQUEST'S TEARDOWN, THEN THE LEVEL.
     *
     * MEASURED, and this is why the fourth mode exists rather than
     * being another guess. `state` on a solo host reported:
     *
     *     action=lsw2_wh_write_state_request(48) ACCEPTED
     *     GUARD went OFF at tick 74 - the level WAS torn down
     *     console healthy, 0 dispatch-miss
     *     VERIFY - slot 0 has no actor now
     *
     * Two things there matter. **The state request performs a REAL TEARDOWN** -
     * the level-holder guard went off, which is exactly what a same-level WARP
     * was measured failing to do, and exactly why that warp double-freed:
     * "the build runs against structures the previous build already owns". And
     * **the console survived**, where the warp killed it.
     *
     * What it does not do is rebuild anybody: state 48 with no level request
     * leaves no actor, which the verify says plainly.
     *
     * So the two halves are complementary here in the same way they were found
     * to be for the lobby return - and the ORDER is the same, for the same reason:
     * both write one request block, so the second erases the first. The state
     * request tears the level down; the level request then rebuilds it, and
     * because the teardown has now actually happened, the same-level
     * double-free hazard should be absent.
     *
     * THAT IS A PREDICTION, and the harness is what tests it. If the console
     * dies with FUN_02055210's signature anyway, the teardown was not the whole
     * story and the lobby respawn is back to needing the guest-thread capability. */
    /* AND A FIFTH: `coop` - DO WHAT THE CO-OP START DOES, ARGUMENT FOR ARGUMENT.
     *
     * This is not another permutation of the four above; it is the cartridge's
     * own sequence, read out of the decompiled co-op entry:
     *
     *     FUN_02088b84(container, level);              hand the container the level
     *     FUN_0205ecc4(..., 4, 0, 0xe, level, 0);      kind 4, MODE 0x0E
     *
     * THE MODE IS THE WHOLE THING. `lsw2_cheats_warp_cold` has always written
     * mode **5**, and statelog.h names 5 `Ordinary` with the note that it has
     * **no arm in FUN_0208c888**. The co-op entry writes **0x0E**, `CoopEnter`,
     * whose note is "reaches the spawner".
     *
     * So the port has been asking for the level in a mode that does not reach
     * the spawner, and the cartridge asks in one that does - which is exactly
     * the observed difference: Start Lobby did not respawn the player the way
     * starting a co-op game does. Four modes failed while the answer
     * was one argument in a call the port already makes.
     *
     * AND IT EXPLAINS THE CRASHES RATHER THAN DODGING THEM. Re-entering your
     * own level was measured to kill the guest - but that was always with
     * mode 5. The co-op start re-enters the same level routinely and survives,
     * and it uses 0x0E. If mode is what selects the arm in the per-frame update,
     * then asking in the mode the game itself uses is not a workaround, it is
     * the road.
     *
     * `FUN_02088b84` is checked and clean on sweep_guestcall_safety - no polling
     * loop on the direct-call graph - which is a weak positive and reason to
     * proceed carefully rather than a guarantee. */
    enum class Mode { Spawn, Warp, State, Pair, Coop };
    Mode mode = Mode::Spawn;
    if (const char* m = std::getenv("LSW2_LOBBY_RESPAWN_MODE")) {
        if (*m == 'w' || *m == 'W') mode = Mode::Warp;
        else if (*m == 'p' || *m == 'P') mode = Mode::Pair;
        else if (*m == 'c' || *m == 'C') mode = Mode::Coop;
        else if (*m == 's' && (m[1] == 't' || m[1] == 'T')) mode = Mode::State;
    }
    const bool want_spawn = mode == Mode::Spawn;

    const char* action =
        mode == Mode::Spawn ? "FUN_02089e60(container)"
        : mode == Mode::Warp ? "lsw2_cheats_warp"
        : mode == Mode::Pair ? "state_request(48) THEN warp_cold(level)"
        : mode == Mode::Coop ? "FUN_02088b84(container,level) THEN "
                               "warp_cold_as(level, 0x0E CoopEnter)"
                             : "lsw2_wh_write_state_request(48)";

    /* THE PAIR IS DEFERRED, NOT DONE HERE. Both halves write one request block,
     * so issuing them in the same tick would mean the second erased the first
     * before the machine ever saw it - that was measured, not assumed. The
     * state request goes in now and the level request is owed; the tick below
     * serves it once the manager is idle again, which is also when the teardown
     * this mode depends on has actually run. */
    /* THE CO-OP SEQUENCE RUNS BOTH HALVES HERE, in the cartridge's order. The
     * container is handed the level first - that is what FUN_02088b84 is for,
     * and lobbyrespawn.h already names it kLsw2PlayerContainerSetLevel - and
     * only then is the level asked for, in CoopEnter. Unlike the `pair` mode
     * these two do not fight over one request block: the first is a call, not
     * a request. */
    bool ok;
    if (mode == Mode::Coop) {
        const bool handed = lsw2_guest_call(lsw2_arm9_FUN_02088b84,
                                            kLsw2PlayerContainerSetLevel,
                                            kLsw2PlayerContainer, level);
        ok = handed && lsw2_cheats_warp_cold_as(int(level),
                                                uint32_t(Lsw2LevelMode::CoopEnter));
        std::fprintf(stderr,
                     "[lobbyrespawn] %s: coop sequence - container set level %s, "
                     "then level %u in mode 0x%02X CoopEnter %s\n",
                     why, handed ? "OK" : "REFUSED", unsigned(level),
                     unsigned(Lsw2LevelMode::CoopEnter),
                     ok ? "REQUESTED" : "not requested");
    } else {
        ok = mode == Mode::Spawn
            ? lsw2_guest_call(lsw2_arm9_FUN_02089e60, kLsw2SpawnDispatcher,
                              kLsw2PlayerContainer)
        : mode == Mode::Warp ? lsw2_cheats_warp(int(level))
                             : lsw2_wh_write_state_request(kLsw2StateDefaultArm);
    }
    if (ok && mode == Mode::Pair) {
        g_owed_level = int(level);
        std::fprintf(stderr,
                     "[lobbyrespawn] %s: state request in; level %u is OWED and "
                     "will be asked for once the manager is idle\n",
                     why, unsigned(level));
    }

    std::fprintf(stderr,
                 "[lobbyrespawn] %s: container spawn index %d%s, slot 0 party "
                 "character %u%s\n",
                 why, int(spawn_index),
                 spawn_index < 0 ? " (-1 - FUN_02089e60 takes its SHORT path)" : "",
                 unsigned(party_char),
                 party_char == 100u ? " (100 = no character - nothing to spawn)" : "");

    if (ram_pointer(actor)) {
        g_verify_why = why;
        g_verify_actor = actor;
        g_verify_pos[0] = bx;
        g_verify_pos[1] = by;
        g_verify_pos[2] = bz;
        g_verify_ticks = kVerifyDelayTicks;
    } else {
        std::fprintf(stderr,
                     "[lobbyrespawn] %s: slot %u has no actor in the array at "
                     "%08X - cannot say whether anything respawned\n",
                     why, slot, unsigned(kLsw2CameraActorArray));
    }

    /* SAY EVERY VALUE. A run that changes nothing has to name WHICH link did
     * not move, and the previous four runs each failed at a different one. */
    std::fprintf(stderr,
                 "[lobbyrespawn] %s: action=%s %s  (level %u, state mode 0x%02X, "
                 "arg_c %u, via_state %d) - verifying in %d ticks\n",
                 why,
                 action,
                 ok ? "ACCEPTED" : "REFUSED",
                 unsigned(level), unsigned(was_mode), unsigned(arg_c),
                 level_via_state, kVerifyDelayTicks);
    if (!ok) return false;
    return true;
}

}  // namespace
