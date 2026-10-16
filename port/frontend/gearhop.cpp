/* See gearhop.h. */

#include "gearhop.h"
#include "vwh.h"

#include "gearapply.h"
#include "headfix.h"
#include "netmsglog.h"   /* kLsw2MsgOwner - the netgame message object */
#include "hudportrait.h"
#include "pauseredraw.h"  /* the pause rebuild's pinned hop */
#include "levelhud.h"     /* the level HUD's messages, checked at take */
#include "partyportrait.h"
#include "ui.h"

#include "runtime_arm.h"
#include "state.h"

#include <cstdio>
#include <cstdlib>

namespace {

/* HOW MANY REQUESTS CAN WAIT. Six customization slots is the whole of what the
 * fork ever asks for at once, so eight is generous; a full queue is REPORTED
 * rather than silently dropped. */
constexpr int kQueue = 8;

/* ONE RECORD FOR ANY GUEST CALL THE HOP CAN RUN. It began as (actor, part) for
 * the gear applier; the portrait's party refresh needs a different target and
 * different arguments, and duplicating the whole mechanism to say so would mean
 * two state machines to keep correct instead of one. */
enum ReqKind { kGear, kRefresh, kPortrait, kExtras, kAction, kSend };

/* The game's netgame send: 0x020B04F0(kLsw2MsgOwner, type, payload, 0xFF), the
 * call every per-player message in the image makes. */
constexpr uint32_t kNetSend = 0x020B04F0u;
struct Request {
    uint32_t target;
    uint32_t a0, a1, a2;
    uint32_t a3;           /* r3, for kAction only - see lsw2_gearhop_request_action */
    ReqKind kind;
    int slot;              /* customization slot, or -1 where there is none */
    uint32_t host;         /* the only override that may take it, or 0 = any -
                              see lsw2_gearhop_request_call_at in gearhop.h */
};

Request g_queue[kQueue];
int g_queued = 0;

enum HopState { kIdle, kRunning };
HopState g_state = kIdle;

/* WHICH HOOK ARMED IT. whsession.cpp's `g_end_host` is the precedent and the
 * reason it exists: two hooks sharing one state machine will otherwise let the
 * wrong one walk into the return branch and restore an lr and r0-r3 that were
 * never saved for it. Cheap here, and the bug it prevents looks like a random
 * wild branch. */
uint32_t g_host = 0;
uint32_t g_saved_lr = 0;
uint32_t g_saved_r[4] = {0, 0, 0, 0};
uint32_t g_saved_sp = 0;

/* The request in flight, kept out of the queue so a completion knows what it
 * completed. */
Request g_active = {0, 0, 0, 0, 0, kGear, -1, 0};

/* HOW LONG AN ARM MAY GO UNCLAIMED. Two seconds of frames. The joiner's
 * session-end hop was measured never coming back, and an arm left standing is a
 * latent wild branch. */
constexpr int kDeadline = 120;
int g_frames = 0;

int g_reports = 0;
constexpr int kReports = 12;

/* COUNTERS - off unless LSW2_TRACE_GEARHOP=1.
 *
 * These settled a question in one run that three rounds of reasoning had not:
 * the hosts were firing twenty times and stopping, which no amount of reading
 * the code had revealed. Kept, because the same question will be asked again. */
int g_trace = -1;
long g_hook_calls = 0;
long g_enqueued = 0;
long g_taken = 0;
long g_completed = 0;
long g_said_at = 0;

bool trace_on() {
    if (g_trace < 0) {
        const char* v = std::getenv("LSW2_TRACE_GEARHOP");
        g_trace = (v && *v == '1') ? 1 : 0;
        if (g_trace)
            std::fprintf(stderr, "[fork] gear hop trace ENABLED\n");
    }
    return g_trace == 1;
}

void trace_beat() {
    if (!trace_on()) return;
    if (g_hook_calls - g_said_at < 10) return;
    g_said_at = g_hook_calls;
    std::fprintf(stderr,
                 "[fork] gear hop trace: hook calls %ld, enqueued %ld, taken "
                 "%ld, completed %ld | queue %d state %d\n",
                 g_hook_calls, g_enqueued, g_taken, g_completed,
                 g_queued, int(g_state));
}

bool same_work(const Request& r, uint32_t target, uint32_t a0, uint32_t a1,
               uint32_t a2, uint32_t a3, ReqKind kind) {
    if (r.target != target || r.a0 != a0 || r.a1 != a1) return false;
    /* An ACTION is one call per slot or part on one actor, so its whole
     * argument list is the work - the older kinds key on (target, a0, a1). */
    return kind != kAction || (r.a2 == a2 && r.a3 == a3);
}

bool enqueue(uint32_t target, uint32_t a0, uint32_t a1, uint32_t a2,
             ReqKind kind, int slot, uint32_t host, uint32_t a3 = 0u) {
    /* A DUPLICATE IS NOT AN ERROR AND NOT A NEW REQUEST. The callers run every
     * frame; without this the queue would fill with the same work. */
    for (int i = 0; i < g_queued; ++i)
        if (same_work(g_queue[i], target, a0, a1, a2, a3, kind)) return false;
    if (g_state != kIdle && same_work(g_active, target, a0, a1, a2, a3, kind))
        return false;

    if (g_queued >= kQueue) {
        if (g_reports < kReports) {
            ++g_reports;
            std::fprintf(stderr,
                         "[fork] gear hop: queue FULL, dropping target %08X - "
                         "this is a lost request, not a quiet skip\n",
                         unsigned(target));
        }
        return false;
    }

    g_queue[g_queued].target = target;
    g_queue[g_queued].a0 = a0;
    g_queue[g_queued].a1 = a1;
    g_queue[g_queued].a2 = a2;
    g_queue[g_queued].a3 = a3;
    g_queue[g_queued].kind = kind;
    g_queue[g_queued].slot = slot;
    g_queue[g_queued].host = host;
    ++g_queued;
    ++g_enqueued;
    if (trace_on())
        std::fprintf(stderr,
                     "[fork] gear hop trace: ENQUEUED target %08X slot %d "
                     "a0 %08X a1 %u (queue now %d)\n",
                     unsigned(target), slot, unsigned(a0), unsigned(a1),
                     g_queued);
    return true;
}

}  // namespace

bool lsw2_gearhop_request(uint32_t actor, uint16_t part, int slot) {
    if (!ui_fork_build() || actor == 0) return false;
    /* NOT `return enqueue(...)`: sweep_log_reachable reads `return f();` as a
     * prototype rather than a call - its head-check allows `=` but not
     * `return` - and would then report enqueue's diagnostics as unreachable.
     * Assigning first is the shape it already understands. */
    const bool queued = enqueue(kLsw2GearApplier, actor, part, 0u, kGear, slot, 0u);
    return queued;
}

/* THE PORTRAIT'S REQUESTS ARE NOT FORK-GATED. The gear applier above serves
 * Free Play customization, a fork-only feature, and keeps its gate. The party
 * refresh and the portrait sequence serve the top-screen portrait following a
 * Free Play change - an Enhancement present in BOTH builds, on by default -
 * and fork.cpp already runs both modules above its fork-only return for
 * exactly that reason. These three gates were the one place the baseline's
 * half of that Enhancement was still refused: the modules ran, asked, and were
 * told no. The hosts they need (0x0202C218, 0x0208CDB0) sit in
 * kOverridesAlways, so the baseline has somewhere to run them. */
bool lsw2_gearhop_request_refresh(uint32_t widget) {
    if (widget == 0) return false;
    const bool queued = enqueue(kLsw2PartyRefresh, widget, 0u, 0u, kRefresh, -1, 0u);
    return queued;
}

bool lsw2_gearhop_request_call(uint32_t target, uint32_t a0, uint32_t a1,
                               uint32_t a2, int tag) {
    if (target == 0) return false;
    const bool queued = enqueue(target, a0, a1, a2, kPortrait, tag, 0u);
    return queued;
}

bool lsw2_gearhop_request_call_at(uint32_t target, uint32_t a0, uint32_t a1,
                                  uint32_t a2, int tag, uint32_t host) {
    if (target == 0) return false;
    const bool queued = enqueue(target, a0, a1, a2, kPortrait, tag, host);
    return queued;
}

bool lsw2_gearhop_request_action(uint32_t target, uint32_t a0, uint32_t a1,
                                 uint32_t a2, uint32_t a3) {
    if (target == 0) return false;
    return enqueue(target, a0, a1, a2, kAction, -1, 0u, a3);
}

bool lsw2_gearhop_request_action_at(uint32_t target, uint32_t a0, uint32_t a1,
                                    uint32_t a2, uint32_t a3, uint32_t host) {
    if (target == 0 || host == 0) return false;
    return enqueue(target, a0, a1, a2, kAction, -1, host, a3);
}

bool lsw2_gearhop_request_send(uint8_t type, uint32_t word0, uint32_t word1) {
    if (type == 0) return false;
    return enqueue(kNetSend, type, word0, word1, kSend, -1, 0u);
}

bool lsw2_gearhop_request_toggle(uint32_t target, uint32_t a0, uint32_t a1,
                                 uint32_t a2) {
    if (!ui_fork_build() || target == 0) return false;
    const bool queued = enqueue(target, a0, a1, a2, kExtras, -1, 0u);
    return queued;
}

void lsw2_gearhop_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): re-dressing an actor on one console is a divergence. */
    if (lsw2_vwh_lockstep()) return;
    if (g_state != kRunning) return;
    if (++g_frames < kDeadline) return;

    g_state = kIdle;
    g_frames = 0;
    std::fprintf(stderr,
                 "[fork] gear hop: the arm at %08X was never claimed within %d "
                 "frames for target %08X. Closing it WITHOUT restoring "
                 "registers - restoring an lr saved for a call that is not "
                 "coming back would be a wild branch\n",
                 unsigned(g_host), kDeadline, unsigned(g_active.target));
}

bool lsw2_gearhop_hook(uint32_t host_entry) {
    /* NO FORK GATE HERE EITHER - see the note above the portrait's requests.
     * In the baseline the only requests that can exist are the portrait's,
     * because the gear
     * applier still refuses outside the fork, so an ungated hook changes
     * nothing the baseline can observe except that a Free Play switch now
     * refreshes its portrait. */
    ++g_hook_calls;
    trace_beat();

    if (g_state == kRunning) {
        /* ONLY THE HOOK THAT ARMED IT MAY COMPLETE IT. */
        if (host_entry != g_host) return false;

        /* IT CAME BACK. R[14] was pointed here, so the cartridge's own function
         * returns into this override; put the host's world back before letting
         * it run its own body.
         *
         * R[0] IS READ FIRST, because the restore below overwrites it and the
         * refresh's answer lives there. */
        const uint32_t result = g_cpu.R[0];
        g_state = kIdle;
        g_frames = 0;
        g_cpu.R[14] = g_saved_lr;
        for (int i = 0; i < 4; ++i) g_cpu.R[i] = g_saved_r[i];
        /* A SEND'S PAYLOAD SLOT GOES BACK: the callee left sp where it found
         * it, eight bytes below the host's. */
        if (g_active.kind == kSend) g_cpu.R[13] = g_saved_sp;
        ++g_completed;

        if (g_reports < kReports) {
            ++g_reports;
            std::fprintf(stderr,
                         "[fork] gear hop: %08X RETURNED on the guest's thread "
                         "(r0 %08X)\n",
                         unsigned(g_active.target), unsigned(result));
        }

        if (g_active.kind == kPortrait) {
            /* THE PORTRAIT. The sequence continues from here rather than inline,
             * because each call lands on a later frame - see gearhop.h. */
            lsw2_hudportrait_step_returned(g_active.slot, result);
        } else if (g_active.kind == kGear) {
            /* THE HEAD UNDER HEADWEAR. The head re-attach has to run AFTER the applier, and
             * before this hop existed the applier never completed, so it was
             * never once reached. */
            lsw2_headfix_after_apply(g_active.a0, uint16_t(g_active.a1));
        } else if (g_active.kind == kRefresh) {
            /* THE PORTRAIT. R[0] IS THE REFRESH'S ANSWER and the cartridge's own
             * callers honour it: zero means it decided there was nothing to
             * show, and acting over that would be a change the game never
             * makes. */
            lsw2_partyportrait_refresh_returned(result);
        }
        /* kExtras: nothing on completion. The cartridge's own Extras toggle
         * (driven from the host settings menu) is fire-and-forget - it flips one flag and runs its own
         * music/letterbox/party-rebuild side effects; the fork does not act on
         * its return. */
        return false;
    }

    if (g_state != kIdle) return false;
    if (g_queued <= 0) return false;

    /* Take the oldest request THIS HOST MAY RUN. A request pinned to another
     * override waits for that override to ask; everything else is first come.
     * A pinned request at the head does not block the ones behind it, because
     * the order between a portrait step and a gear part was never a promise -
     * only the order WITHIN the portrait's own sequence is, and all three of
     * its steps carry the same pin. */
    int pick = -1;
    for (int i = 0; i < g_queued; ++i) {
        if (g_queue[i].host == 0u || g_queue[i].host == host_entry) {
            /* A PORTRAIT STEP IS CHECKED AGAINST THE HUD ON SCREEN FIRST -
             * see lsw2_hudportrait_step_still_live. Only the steps pinned to
             * the HUD's own frame step are the portrait's; the gear applier
             * queues the same kind unpinned and is not asked. */
            if (g_queue[i].kind == kPortrait && g_queue[i].host == kLsw2HudStep &&
                !lsw2_hudportrait_step_still_live(g_queue[i].slot, g_queue[i].a0)) {
                const int step = g_queue[i].slot;
                for (int k = i + 1; k < g_queued; ++k) g_queue[k - 1] = g_queue[k];
                --g_queued;
                lsw2_hudportrait_step_dropped(step);
                --i;
                continue;
            }
            /* AND A PAUSE REBUILD AGAINST THE MENU UPDATING NOW - its r0, since
             * the host has not run a single instruction yet (pauseredraw.h). */
            if (g_queue[i].host == kLsw2PauseStateUpdate &&
                g_queue[i].target == kLsw2PauseRebuild &&
                !lsw2_pause_rebuild_still_live(g_queue[i].a0, g_cpu.R[0])) {
                for (int k = i + 1; k < g_queued; ++k) g_queue[k - 1] = g_queue[k];
                --g_queued;
                --i;
                continue;
            }
            /* AND A HUD MESSAGE AGAINST THE LEVEL UPDATING NOW (levelhud.h). */
            if (g_queue[i].host == kLsw2HudStep &&
                g_queue[i].target == kLsw2LevelStateMessage &&
                !lsw2_levelhud_message_still_live(g_queue[i].a0, g_cpu.R[0])) {
                for (int k = i + 1; k < g_queued; ++k) g_queue[k - 1] = g_queue[k];
                --g_queued;
                --i;
                continue;
            }
            pick = i;
            break;
        }
    }
    if (pick < 0) return false;
    g_active = g_queue[pick];
    for (int i = pick + 1; i < g_queued; ++i) g_queue[i - 1] = g_queue[i];
    --g_queued;

    ++g_taken;
    g_state = kRunning;
    g_frames = 0;
    g_host = host_entry;
    g_saved_lr = g_cpu.R[14];
    for (int i = 0; i < 4; ++i) g_saved_r[i] = g_cpu.R[i];

    g_cpu.R[0] = g_active.a0;
    g_cpu.R[1] = g_active.a1;
    g_cpu.R[2] = g_active.a2;
    /* R3 ONLY FOR AN ACTION: the older kinds have always left the host's r3 in
     * place, and changing what their targets see is not this change's to make. */
    if (g_active.kind == kAction) g_cpu.R[3] = g_active.a3;
    /* A SEND: the payload's two words go just below the host's sp - free stack,
     * since the host has not run its prologue and interrupts have their own -
     * and the send copies it before it returns (0x020B05A8). */
    if (g_active.kind == kSend) {
        g_saved_sp = g_cpu.R[13];
        g_cpu.R[13] = g_saved_sp - 8u;
        bus_device_write32(0, g_cpu.R[13], g_active.a1);
        bus_device_write32(0, g_cpu.R[13] + 4u, g_active.a2);
        g_cpu.R[0] = kLsw2MsgOwner;
        g_cpu.R[1] = g_active.a0;
        g_cpu.R[2] = g_cpu.R[13];
        g_cpu.R[3] = 0xFFu;
    }
    g_cpu.R[14] = host_entry;
    g_cpu.R[15] = g_active.target;

    if (g_reports < kReports) {
        ++g_reports;
        std::fprintf(stderr,
                     "[fork] gear hop: taken - the guest is now inside %08X "
                     "(a0 %08X a1 %u) and will return to %08X\n",
                     unsigned(g_active.target), unsigned(g_active.a0),
                     unsigned(g_active.a1), unsigned(host_entry));
    }
    return true;
}
