/* See statelog.h. Read the state, say when it changes. */

#include "statelog.h"

#include "state.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace {

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

int g_last = -2;    /* -2 so the first observation always prints */

/* A SHORT TRAIL, because the sequence is the evidence. "state 20 appeared" says
 * little; "9 -> 34 -> 20 after walking into the console" names it. Eight is
 * enough to cover a menu walk and short enough to read on one line. */
constexpr int kTrail = 8;
int g_trail[kTrail];
int g_trail_n = 0;

}  // namespace

int lsw2_state_current() {
    const uint32_t obj = bus_device_read32(0, kLsw2StateManager + kLsw2StateCurrent);
    if (obj < kRamLow || obj >= kRamHigh) return -1;
    return int(bus_device_read32(0, obj + kLsw2StateId));
}

int lsw2_state_pending() {
    const uint32_t id = bus_device_read32(
        0, kLsw2StateManager + kLsw2ScreenSlot * kLsw2SlotStride + kLsw2SlotArgs);
    return (id > 0 && id < uint32_t(kLsw2StateCount)) ? int(id) : -1;
}

/* See statelog.h. The one copy of the read that four modules disagreed about. */
uint32_t lsw2_state_current_object() {
    const uint32_t obj = bus_device_read32(0, kLsw2StateManager + kLsw2StateCurrent);
    if (obj < kRamLow || obj >= kRamHigh) return 0;

    /* THE EVIDENCE, PRINTED ONCE, because four modules are being judged against
     * this one and an inference is not enough to convict them.
     *
     * This prints BOTH chains side by side the first time a state object is
     * seen: the one-read form used here, and the deref-first form the other
     * four use. If the second reads a different object - or nothing - then the
     * bug is confirmed by measurement rather than by argument, in the same run
     * that the fix is verified. Control and test together, which is the shape
     * this project keeps having to relearn.
     *
     * Independent evidence already in hand: whsession's own working write
     * reported "the machine's own request block at 02102968", and
     * 0x02102968 - 0x02102900 = 0x68, so the constant is a BASE. This line is
     * the direct confirmation. */
    static bool said = false;
    if (!said) {
        said = true;
        const uint32_t deref = bus_device_read32(0, kLsw2StateManager);
        uint32_t via_deref = 0;
        if (deref >= kRamLow && deref < kRamHigh)
            via_deref = bus_device_read32(0, deref + kLsw2StateCurrent);
        std::fprintf(stderr,
                     "[state] object chain: base+off -> %08X (id %02X) | "
                     "deref-first -> mgr %08X then %08X\n",
                     unsigned(obj),
                     unsigned(bus_device_read32(0, obj + kLsw2StateId) & 0xFFu),
                     unsigned(deref), unsigned(via_deref));
    }
    return obj;
}

void lsw2_statelog_tick() {
    /* BOTH BUILDS, DELIBERATELY - and this was wrong when first written.
     *
     * The gate here was `if (!ui_fork_build()) return;`, copied from the other
     * probes, and it made the log unable to see the one thing it exists for.
     * The two states being hunted are the pause row's wireless target and the
     * co-op screen, and NEITHER is reachable in the fork: the fork's boot warps
     * past the Cantina to the arena lobby, so the console is never walked to,
     * and the fork's trimmed pause menu hides the wireless row, so `lsw2_pause_escape` guarantees the cursor
     * never rests on it. A fork-gated state log watches the one build in which
     * both of its targets have been removed.
     *
     * The other probes gate on the fork because they read fork state. This one
     * reads the cartridge's, which the baseline has more of. */
    const int now = lsw2_state_current();
    if (now == g_last) return;
    g_last = now;

    if (g_trail_n < kTrail) {
        g_trail[g_trail_n++] = now;
    } else {
        for (int i = 1; i < kTrail; ++i) g_trail[i - 1] = g_trail[i];
        g_trail[kTrail - 1] = now;
    }

    char trail[128];
    int n = 0;
    for (int i = 0; i < g_trail_n && n < int(sizeof trail) - 8; ++i) {
        n += std::snprintf(trail + n, sizeof trail - size_t(n), "%s%d",
                           i ? " -> " : "", g_trail[i]);
    }

    /* THE RANGE MATTERS. The factory bounds the id at 0x39 and sends anything
     * above it to the default arm, so a state outside the range is the game
     * asking for something it does not implement - worth seeing, not hiding. */
    const char* note = (now < 0)              ? "  (no current state object)"
                     : (now >= kLsw2StateCount) ? "  (OUTSIDE the factory's range)"
                                                : "";

    /* `[state]`, NOT `[fork]`: this one prints in the baseline too, and a tag
     * naming the wrong build is the kind of small lie a reader later builds a
     * wrong conclusion on. */
    /* THE WORD "entered" EARNS ITS PLACE. `tools/sweep_runbook.py` proves a
     * quoted log line exists by matching its literal prefix up to the first
     * placeholder, and skips any stem under twelve characters as too weak to
     * be evidence. `[state] %d` has a seven-character stem, so the runbook
     * step pointing at this line would have been silently unverified - the
     * exact class of drift that gate exists to catch. */
    /* THE TIMESTAMP GOES LAST, ON PURPOSE. `sweep_runbook.py` and every grep in
     * the run recipe match this line by its literal PREFIX, so anything added
     * in front of "[state] entered" would silently unverify them. */
    std::fprintf(stderr, "[state] entered %d%s   trail: %s  t=%s\n", now, note,
                 trail, lsw2_log_clock());
}

/* See statelog.h.
 *
 * THE LOCAL-TIME OFFSET IS TAKEN ONCE. This used to call localtime_s on every
 * line, and on this CRT that re-reads the timezone each call: measured at ~9 ms
 * a call. The WH report stamps a line each time the step signal
 * changes, which in a data-sharing session is several times a second, so the
 * clock alone cost ~90 ms of every second on the host. A log stamp does not
 * need to follow a daylight-saving change mid-session; the offset read at the
 * first line is kept for the run. */
const char* lsw2_log_clock() {
    static char buf[16];
    static const long long offset_s = [] {
        const std::time_t t = std::time(nullptr);
        std::tm local{}, utc{};
#if defined(_WIN32)
        localtime_s(&local, &t);
        gmtime_s(&utc, &t);
#else
        localtime_r(&t, &local);
        gmtime_r(&t, &utc);
#endif
        long long d = (local.tm_hour - utc.tm_hour) * 3600LL +
                      (local.tm_min - utc.tm_min) * 60LL +
                      (local.tm_sec - utc.tm_sec);
        const int day = local.tm_yday - utc.tm_yday;
        if (day == 1 || day < -1) d += 86400;       /* local is a day ahead  */
        else if (day == -1 || day > 1) d -= 86400;  /* local is a day behind */
        return d;
    }();
    const auto now = std::chrono::system_clock::now();
    const long long ms_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    long long sod = (ms_epoch / 1000 + offset_s) % 86400;
    if (sod < 0) sod += 86400;
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", int(sod / 3600),
                  int(sod / 60 % 60), int(sod % 60), int(ms_epoch % 1000));
    return buf;
}
