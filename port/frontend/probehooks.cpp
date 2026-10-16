/* See probehooks.h. */

#include "probehooks.h"

#include "cheats.h"
#include "levels.h"
#include "ui.h"

#include "runtime_arm.h"
#include "state.h"

#include <cstdio>
#include <cstdlib>

/* THE DISPATCH HISTOGRAM, from port/patches/0010-dispatch-histogram.patch.
 *
 * Declared here rather than in a framework header so the patch touches exactly
 * one file. It counts the PCs runtime_dispatch is actually consulted for, which
 * is precisely the set an override can hook - the branch ring cannot tell those
 * apart from ordinary branches, which is why seven hook candidates had to be
 * refuted one build at a time. */
extern "C" void runtime_dispatch_hist_reset(void);
extern "C" void runtime_dispatch_hist_dump(uint32_t top);

namespace {

/* HOW MANY FRAMES TO WATCH. Two is enough to tell "every frame" from "once":
 * a per-frame address appears twice, a one-shot appears once. Three gives a
 * little margin without letting deep trace run long enough to matter. */
constexpr int kWatchFrames = 3;

/* THE RING. Deep trace records every branch on both cores, so a few frames of
 * a busy level is a lot of entries; this is the largest single copy worth
 * taking, and the report says plainly if it filled up. */
constexpr int kRingEntries = 8192;

/* HOW MANY TO REPORT. The interesting answer is the handful of addresses that
 * recur most, not the whole trace. */
constexpr int kReportTop = 40;

int g_on = -1;
int g_settle = 0;
int g_frames = -1;   /* -1 = not armed yet, >= 0 = counting down */
bool g_done = false;

bool probe_on() {
    if (g_on < 0) {
        const char* v = std::getenv("LSW2_PROBE_FRAMEPCS");
        g_on = (v && *v == '1') ? 1 : 0;
        if (g_on)
            std::fprintf(stderr,
                         "[hle] frame-PC probe ENABLED - it will arm once the "
                         "lobby settles and report a few frames later\n");
    }
    return g_on == 1;
}

RuntimeTraceEntry g_ring[kRingEntries];

/* One pass, counting distinct PCs. No sorting library, no allocation: the ring
 * is bounded and this runs once. */
struct Bucket { uint32_t pc; int count; };
Bucket g_hist[2048];
int g_hist_used = 0;

void tally(uint32_t pc) {
    for (int i = 0; i < g_hist_used; ++i) {
        if (g_hist[i].pc == pc) { ++g_hist[i].count; return; }
    }
    if (g_hist_used < int(sizeof g_hist / sizeof g_hist[0])) {
        g_hist[g_hist_used].pc = pc;
        g_hist[g_hist_used].count = 1;
        ++g_hist_used;
    }
}

}  // namespace

void lsw2_probe_hook_rate(const char* tag) {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LSW2_PROBE_HOOKRATE");
        on = (v && *v == '1') ? 1 : 0;
        /* SAY THAT THE INSTRUMENT IS LIVE. Without this line a silent log is
         * ambiguous - "no hook fires often" and "the probe was never switched
         * on" look identical, and a conclusion was once published from this
         * counter before it had printed anything at all. */
        std::fprintf(stderr, "[hle] hook-rate probe %s\n",
                     on ? "ENABLED" : "off (LSW2_PROBE_HOOKRATE unset)");
    }
    if (on != 1) return;

    /* One slot per tag, matched by POINTER - every caller passes a string
     * literal, so the pointers are stable and this needs no strcmp. */
    struct Slot { const char* tag; long calls; long said; };
    static Slot slots[16] = {};
    Slot* me = nullptr;
    for (int i = 0; i < 16; ++i) {
        if (slots[i].tag == tag) { me = &slots[i]; break; }
        if (slots[i].tag == nullptr) { slots[i].tag = tag; me = &slots[i]; break; }
    }
    if (!me) return;

    ++me->calls;
    /* AND SAY THAT EACH TAG ARRIVED AT ALL, once. A hook that fires five times
     * and a hook that never fires are different answers, and the 60-call report
     * alone cannot tell them apart. */
    if (me->calls == 1)
        std::fprintf(stderr, "[hle] hook FIRST CALL: %s (level %d)\n",
                     tag, lsw2_cheats_current_level());

    if (me->calls - me->said >= 60) {
        me->said = me->calls;
        std::fprintf(stderr, "[hle] hook rate: %-24s %ld calls (level %d)\n",
                     tag, me->calls, lsw2_cheats_current_level());
    }
}

void lsw2_probe_frame_pcs() {
    if (g_done || !probe_on() || !ui_fork_build()) return;

    /* WAIT FOR ORDINARY PLAY. The point is what runs in the lobby every frame,
     * not what runs during a level load - a boot trace would be dominated by
     * one-shot setup and would answer a different question. */
    if (g_frames < 0) {
        if (lsw2_cheats_current_level() != kLsw2ArenaLobbyLevel) return;
        if (++g_settle < 120) return;          /* two seconds of settled lobby */
        g_frames = kWatchFrames;
        runtime_set_deep_trace(1);
        runtime_trace_reset();
        /* AND THE DISPATCH HISTOGRAM over the same window. The two answer
         * different questions and are worth having side by side: the ring says
         * what EXECUTED, the histogram says what was DISPATCHED. Only the
         * second is hookable. */
        runtime_dispatch_hist_reset();
        std::fprintf(stderr,
                     "[hle] frame-PC probe: armed for %d frames of settled "
                     "lobby\n", kWatchFrames);
        return;
    }

    if (--g_frames > 0) return;

    /* THE HOOKABLE SET FIRST, because it is the one items 2 and 3 need. A PC
     * here that appears about once per frame is a per-frame hook candidate; one
     * that appears hundreds of times is an inner loop and no use. */
    /* ALL of them, not a top-40: the hook being hunted appears about ONCE PER
     * FRAME, so it sits near the BOTTOM of a frequency ranking. The head of
     * the list is inner loops and is exactly what is not wanted. */
    runtime_dispatch_hist_dump(4000);

    /* DISARM FIRST, then read. Leaving deep trace on is what runtime_arm.h
     * warns about, and the copy below is not instant. */
    runtime_set_deep_trace(0);
    g_done = true;

    const uint32_t n = runtime_trace_copy_recent(g_ring, kRingEntries);
    for (uint32_t i = 0; i < n; ++i) tally(g_ring[i].pc);

    std::fprintf(stderr,
                 "[hle] frame-PC probe: %u ring entries over %d frames, %d "
                 "distinct addresses%s\n",
                 unsigned(n), kWatchFrames, g_hist_used,
                 n >= kRingEntries ? "  (RING FULL - the window may be "
                                     "truncated, treat counts as a lower "
                                     "bound)" : "");

    /* SELECTION SORT over the top few, because only the head of the list is
     * being reported and the array is small. */
    for (int slot = 0; slot < kReportTop && slot < g_hist_used; ++slot) {
        int best = slot;
        for (int i = slot + 1; i < g_hist_used; ++i)
            if (g_hist[i].count > g_hist[best].count) best = i;
        const Bucket t = g_hist[slot];
        g_hist[slot] = g_hist[best];
        g_hist[best] = t;
        std::fprintf(stderr, "[hle]   %2d  pc=%08X  x%d\n",
                     slot, unsigned(g_hist[slot].pc), g_hist[slot].count);
    }
}
