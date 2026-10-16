/* See countboot.h. The game's own bootstrap, with the fork's count. */

#include "countboot.h"
#include "vwh.h"
#include "netcount.h"

#include "forksession.h"
#include "guestcall.h"
#include "ui.h"
#include "viewmask.h"

#include "state.h"
/* nds_cpu_halted - the terminal-halt condition that refused every call this
 * file made from the between-frames tick. See the hold below. */
#include "io.h"

#include <cstdio>

extern "C" {
void lsw2_arm9_FUN_0205ce5c(void);   /* the input count byte  */
void lsw2_arm9_FUN_020730d0(void);   /* the slot bound        */
void lsw2_arm9_FUN_02073054(void);   /* the player count      */
}

namespace {

constexpr uint32_t kInputCountSetter = 0x0205CE5Cu;
constexpr uint32_t kBoundSetter = 0x020730D0u;
constexpr uint32_t kCountSetter = 0x02073054u;

/* The object `0x0205CE5C` writes into - it takes the object in r0 and the
 * count in r1, unlike the other two which take only a value. */
/* See netcount.h - kLsw2NetCountObject is the one copy. */

bool g_armed = false;

/* How many frames the request has been waiting for an unhalted guest, and how
 * many it is allowed to. See the hold in the tick. */
int g_waited = 0;
constexpr int kWaitFrames = 600;

}  // namespace

void lsw2_countboot_arm() { g_armed = true; }

void lsw2_countboot_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): the netgame sets its own player count in a session; a setter called from one console desyncs it. */
    if (lsw2_vwh_lockstep()) return;
    /* IT ARMS ITSELF NOW, and the measurement that settles which route is
     * right has finally been taken - two instances, live.
     *
     * This ran only from an ImGui button, deliberately, because countboot.h
     * could not say whether the game's own derivation was going to run in the
     * arena lobby and overwrite whatever this wrote. The note below still names
     * the discriminator it was waiting for: a bound that holds means the
     * derivation is not running and this route is the answer.
     *
     * MEASURED, with a host and a joiner connected and the fork reporting
     * "view mask published 0x3 (2 seated)":
     *
     *     view manager [0x021092F4] = 0x00000000    <- there is no manager
     *     pump gate    [0x020FEB88] = 0xFFFFFFFF    <- never set, -1
     *     cached mask  [0x02104D2E] = 0x0000
     *     popcount     [0x02104D32] = 0
     *     count byte   [0x02102821] = 1
     *
     * unchanged before and after the join. The derivation at 0x02071B9C hangs
     * off a state vtable and works on that view manager; with the pointer null
     * it cannot run at all, so the mask the port publishes is never popcounted
     * into anything. That is why the pump's gate stays at -1, why
     * `CMP r0,#0 / BLE done` at 0x020B0428 turns the receive pump off, and why
     * every round start was refused.
     *
     * SO THE PORT MAKES THE CALL THE GAME WOULD HAVE MADE. Not a write - the
     * three functions below are the cartridge's own setters, invoked with the
     * count the cartridge's own popcount would have produced. 0x02073054 also
     * reconfigures the camera manager when [0x027E02A8] & 0x40 is set, which is
     * exactly the reason netcount.h gives for never writing 0x020FEB88
     * directly; calling the setter keeps that behaviour instead of skipping it.
     * (Measured clear in the lobby, so today it is only the store - but this is
     * correct in both cases rather than in the one that happens to hold.)
     *
     * ==== AND THEN IT WAS ARMED AUTOMATICALLY, AND IT BROKE THE GAME ========
     * | On the rising edge of the seated count reaching two, this fired, the |
     * | halt hold below let it land, and all three setters returned:         |
     * |                                                                     |
     * |     count bootstrap: guest awake after 15 frames - seating now       |
     * |     count bootstrap: seated 2, calls ok/ok/ok, bound 4 -> 2          |
     * |                                                                     |
     * | Seconds later the host was spewing                                   |
     * |                                                                     |
     * |     [bus] ARM9 unmapped read 0xE39CC994 w=4                          |
     * |                                                                     |
     * | at wandering addresses and had stopped answering its debug socket.   |
     * | The seam restores g_cpu, so this is not clobbered registers - it is  |
     * | the guest MEMORY these setters write. netcount.h called it in        |
     * | advance: the count byte "sits in the same word as +0x2A, the local   |
     * | station index, and it bounds the four input records it lives among.  |
     * | Writing a count the game disagrees with is a way to make input read  |
     * | from the wrong player." Telling the cartridge there are two stations |
     * | while its own bookkeeping has initialised none makes it walk records |
     * | that do not exist.                                                   |
     * |                                                                     |
     * | The route is therefore NOT safe to fire on its own, and the value    |
     * | being written is not the problem - the state it is written into is.  |
     * | It stays behind the button, which is where it was, and this note is  |
     * | why it is not being tried automatically a second time.               |
     * ======================================================================= */

    if (!g_armed) return;

    /* NOT WHILE THE GUEST IS HALTED. Measured, and it is why the
     * first version of this fired and achieved nothing:
     *
     *     [guestcall] 0205CE5C refused before entry: halted=1 dma_stalled=0
     *     [guestcall] 020730D0 refused before entry: halted=1 ...
     *     [guestcall] 02073054 refused before entry: halted=1 ...
     *     [fork] count bootstrap: seated 2, calls FAILED/FAILED/FAILED
     *
     * Every lifted function begins with `if (runtime_should_yield()) return;`,
     * and runtime_arm.h lists a HALTED cpu among that predicate's terminal-halt
     * conditions. The fork tick runs BETWEEN FRAMES, where the guest is usually
     * sitting in a HALT waiting for VBlank - so a call made from here is
     * refused at the door, before a single instruction, with r15 still at the
     * entry address. That is exactly what the three setters reported.
     *
     * STAYING ARMED IS THE FIX, not moving the call. The tick does run on
     * frames where the guest is awake - the customization dressing and the
     * party-portrait refresh both
     * return from this same seam - so the request simply waits for one of them
     * instead of being spent on the first tick after a player seats. Nothing is
     * forced and nothing is retried into a state that cannot take it.
     *
     * BOUNDED, because "stays armed" and "spins forever" differ by a counter.
     * Ten seconds at 60fps: far longer than a frame's halt, short enough that a
     * bootstrap that can never run says so instead of retrying in silence. */
    if (nds_cpu_halted(NDS_ARM9)) {
        if (++g_waited == 1)
            std::fprintf(stderr, "[fork] count bootstrap: the guest is halted "
                                 "- holding the request until it is awake\n");
        if (g_waited < kWaitFrames) return;
        g_armed = false;
        g_waited = 0;
        std::fprintf(stderr,
                     "[fork] count bootstrap: GAVE UP - the guest stayed "
                     "halted for %d frames, so the seating never ran\n",
                     kWaitFrames);
        return;
    }

    if (g_waited)
        std::fprintf(stderr, "[fork] count bootstrap: guest awake after %d "
                             "frames - seating now\n", g_waited);
    g_waited = 0;
    g_armed = false;

    if (!ui_fork_build()) {
        std::fprintf(stderr, "[fork] count bootstrap: fork build only\n");
        return;
    }

    const Lsw2ForkSession& session = lsw2_fork_session();
    int count = session.player_count();
    if (count < 2) {
        /* THE GAME'S OWN SEQUENCE USES 2, and below that there is nothing to
         * bootstrap - a lone machine is the state the cartridge already starts
         * in. Refusing is more honest than writing 1 and calling it a session. */
        std::fprintf(stderr,
                     "[fork] count bootstrap: only %d player(s) - nothing to "
                     "seat\n", count);
        return;
    }
    if (count > 4) count = 4;

    const int before_bound = lsw2_viewmask_bound();

    const bool a = lsw2_guest_call(lsw2_arm9_FUN_0205ce5c, kInputCountSetter,
                                   kLsw2NetCountObject, uint32_t(count));
    const bool b = lsw2_guest_call(lsw2_arm9_FUN_020730d0, kBoundSetter,
                                   uint32_t(count));
    const bool c = lsw2_guest_call(lsw2_arm9_FUN_02073054, kCountSetter,
                                   uint32_t(count));

    const int after_bound = lsw2_viewmask_bound();

    std::fprintf(stderr,
                 "[fork] count bootstrap: seated %d, calls %s/%s/%s, "
                 "bound %d -> %d\n",
                 count, a ? "ok" : "FAILED", b ? "ok" : "FAILED",
                 c ? "ok" : "FAILED", before_bound, after_bound);

    /* WHAT TO WATCH NEXT. If the bound moved and then goes back to 0 a moment
     * later, the derivation IS running and overwriting this - which means the
     * derivation-driven account holds for this state, and the mask publisher is
     * the answer. If it
     * holds, the derivation is not running here and this route is. */
    std::fprintf(stderr,
                 "   watch the next 'view mask' line: a bound that stays is "
                 "this route working; one that returns to 0 means the "
                 "derivation is live and the mask is the lever\n");
}
