/* The guest half of the host settings' Extras application - see extrasapply.h. */

#include "extrasapply.h"
#include "vwh.h"

#include "cheats.h"
#include "gearhop.h"
#include "forksession.h"
#include "hostsettings.h"
#include "ui.h"

#include <cstdio>
#include <cstdlib>

namespace {

uint32_t g_applied = kLsw2ExtrasNever;

/* DEFAULT ON FOR THE FORK. A theme Extra (Disco, Old Time, Old West, Medieval)
 * only ENTERS PLAY through the cartridge's own toggle FUN_02086810: the raw
 * bit-set merely flips the +0x4518 enabled bit that the 26 per-frame readers
 * test, but the theme MUSIC, the Letterbox, the ten-theme mutual-exclusion and
 * - the visible essence of Old West/Medieval - the party RE-DRESS into the
 * theme's costumes are all side effects of the toggle, which the direct write
 * skips (cheats.cpp:597, cheats.h). So a host enabling Old West on the raw path
 * would set a flag and change nothing on screen. The toggle path is a
 * guest-thread call, and it is VERIFIED SAFE through the complete two-console
 * flow - boot, admission, match entry (the party re-dress that was the last
 * risk), a full match and the lobby return, 0 dispatch-miss, and across single,
 * multiple and mutually-exclusive extras including Old West. It
 * routes through the ROM's own function, so what it draws IS the game's own
 * behaviour by construction. It first shipped opt-in only; Extras chosen in the
 * host settings must actually enter play, so the fork
 * now defaults to it. LSW2_EXTRAS_TOGGLE=0 forces the raw bit-set path back for
 * debugging; =1 (or any other value) keeps it on. */
bool use_toggle() {
    static const int env = [] {
        const char* v = std::getenv("LSW2_EXTRAS_TOGGLE");
        if (!v) return -1;               /* unset  -> follow the build default */
        return std::atoi(v) != 0 ? 1 : 0; /* =0 off, anything else on */
    }();
    if (env >= 0) return env != 0;       /* explicit override wins */
    return ui_fork_build();              /* default: on in the fork, off baseline */
}

/* EXTRAS IN A SESSION - the game's own way, from the host only.
 *
 * A toggle made from one console's tick lands at a different step on each
 * console, so the offline path below cannot run in a session. The
 * game has its own route: its Extras menu, with the multi-view bit up, sends
 * message 0x36 carrying the flag's index instead of toggling (0x02085264), and
 * every console's handler (0x020B2BF0) runs the full toggle 0x02086810 - music,
 * letterbox, party re-dress and the themes' exclusion - at the same exchange.
 * So the host sends that message for each flag its settings want changed.
 *
 * A TOGGLE IS ONLY RIGHT BECAUSE EVERY CONSOLE HOLDS THE SAME FLAGS. A client
 * takes the host's set through the offline path while it connects - before it
 * is seated and sharing datasets - so from the first shared dataset every
 * console's flags agree, and the same toggles keep them agreeing. One flag per
 * message, and the next only once the last has been seen to land (or given up
 * on, for a theme the game's exclusion will not let stand). */
constexpr int kSessionWaitFrames = 45;
constexpr int kSessionMaxTries = 6;
constexpr uint8_t kMsgExtraToggle = 0x36u;
int g_session_wait = 0;
uint32_t g_session_mask = kLsw2ExtrasNever;
int g_session_tries[kLsw2ExtrasCount] = {0};

void session_extras_tick() {
    if (!ui_fork_build() || !lsw2_vwh_hosting()) return;

    /* LSW2_TEST_HOST_EXTRA=<index> - A TEST INSTRUMENT, off unless set: ten
     * seconds into the shared session the host flips that Extra in its own
     * settings, as Change Settings would, so the route below can be measured
     * without driving the settings menu. */
    static const int test_extra = [] {
        const char* v = std::getenv("LSW2_TEST_HOST_EXTRA");
        return v ? std::atoi(v) : -1;
    }();
    static int test_frames = 0;
    if (test_extra >= 0 && test_extra < kLsw2ExtrasCount && ++test_frames == 600) {
        Lsw2HostSettings& hs = lsw2_fork_session().settings();
        hs.extras[test_extra] = !hs.extras[test_extra];
        std::fprintf(stderr, "[fork] LSW2_TEST_HOST_EXTRA: the host flips Extra "
                     "%d to %d\n", test_extra, hs.extras[test_extra] ? 1 : 0);
    }

    const Lsw2HostSettings& s = lsw2_fork_session().settings();
    const uint32_t mask = lsw2_host_extras_mask(s);
    if (mask != g_session_mask) {
        g_session_mask = mask;
        for (int i = 0; i < kLsw2ExtrasCount; ++i) g_session_tries[i] = 0;
        g_session_wait = 0;
    }
    if (g_session_wait > 0) {
        --g_session_wait;
        return;
    }
    for (int i = 0; i < kLsw2ExtrasCount; ++i) {
        if (g_session_tries[i] >= kSessionMaxTries) continue;
        const int cur = lsw2_cheats_extra_enabled(i);
        if (cur < 0) return;
        if (cur == (s.extras[i] ? 1 : 0)) continue;
        if (!lsw2_gearhop_request_send(kMsgExtraToggle, uint32_t(i), 0u)) return;
        g_session_wait = kSessionWaitFrames;
        if (++g_session_tries[i] == kSessionMaxTries)
            std::fprintf(stderr, "[fork] extras in a session: flag %d will not "
                         "hold (the game's theme exclusion overrules it)\n", i);
        else
            std::fprintf(stderr, "[fork] extras in a session: flag %d -> %d, sent "
                         "as the game's own message 0x36 so every console toggles "
                         "it at the same exchange\n", i, s.extras[i] ? 1 : 0);
        return;
    }
}

}  // namespace

void lsw2_extras_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): extras toggled from one console's
     * tick land at different steps - in a session the host sends
     * the game's own message instead (above). */
    if (lsw2_vwh_lockstep()) {
        session_extras_tick();
        return;
    }
    const bool fork = ui_fork_build();
    const Lsw2HostSettings& s = lsw2_fork_session().settings();
    const uint32_t mask = lsw2_host_extras_mask(s);

    if (!lsw2_extras_should_write(mask, g_applied, fork)) return;

    if (use_toggle()) {
        /* THE PARTY MUST EXIST FIRST. Measured 2026-09-13: the toggle re-dresses
         * the party (FUN_02088b84), and at boot (state 15) there is no party -
         * `lsw2_cheats_party_actor(0)` is 0 - so the re-dress ran against nothing
         * and the guest dispatched to garbage (pc=0xE35E0000). The raw bit-set
         * is safe that early because it only sets a bit; the toggle is not. So
         * the toggle path waits for a real party actor, which the lobby and a
         * match have and boot does not. The direct default path is unchanged. */
        const uint32_t actor = lsw2_cheats_party_actor(0);
        if (actor < 0x02000000u || actor >= 0x02400000u) return;

        /* THE CARTRIDGE'S OWN TOGGLE, one flip per differing bit, on the guest
         * thread. Queue every bit that differs each frame - the hop dedups a
         * repeat and drains one per frame, so this settles over a few frames -
         * and only record the mask as applied once nothing differs, so the
         * theme music and party re-dress have actually run.
         *
         * GIVE UP ON A BIT THAT WILL NOT STICK. The ten theme flags are
         * MUTUALLY EXCLUSIVE in the game (0x02086810 clears the group before
         * setting one), so a host that picked TWO themes has each toggle clear
         * the other - both differ forever and this would re-queue every frame.
         * A per-bit attempt budget stops that: after kMaxTries flips that do
         * not make the bit stick, the bit is abandoned (the game's exclusion
         * has overruled the host's wish, which is exactly "every combination the
         * game itself can hold"). The budget resets when the wanted mask
         * changes, so a corrected host setting is tried afresh. */
        constexpr int kMaxTries = 6;
        static uint32_t tried_for_mask = kLsw2ExtrasNever;
        static int tries[kLsw2ExtrasCount] = {0};
        if (tried_for_mask != mask) {
            tried_for_mask = mask;
            for (int i = 0; i < kLsw2ExtrasCount; ++i) tries[i] = 0;
        }

        int pending = 0;
        for (int i = 0; i < kLsw2ExtrasCount; ++i) {
            if (tries[i] >= kMaxTries) continue;   /* abandoned - see above */
            if (lsw2_cheats_toggle_extra_if_needed(i, s.extras[i])) {
                ++pending;
                if (++tries[i] == kMaxTries)
                    std::fprintf(stderr,
                                 "[fork] extras: flag %d will not hold (the "
                                 "game's theme exclusion overrules it); leaving "
                                 "it to the cartridge\n", i);
            }
        }
        if (pending == 0) {
            g_applied = mask;
            std::fprintf(stderr,
                         "[fork] extras applied through the cartridge toggle: "
                         "mask %05X\n", unsigned(mask));
        }
        return;
    }

    /* THE GUEST MAY NOT BE READY, and that is not a failure to report every
     * frame. `lsw2_cheats_set_extras` refuses until the game has built its
     * context, which is most of the boot - so leave `g_applied` alone and try
     * again next frame rather than recording a write that did not happen. */
    if (!lsw2_cheats_set_extras(s.extras, kLsw2ExtrasCount)) return;

    g_applied = mask;
    std::fprintf(stderr, "[fork] extras applied: mask %05X\n", unsigned(mask));
}
