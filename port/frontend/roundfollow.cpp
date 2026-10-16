/* See roundfollow.h. */

#include "roundfollow.h"

#include "cheats.h"
#include "forksession.h"
#include "levels.h"
#include "ui.h"

#include <cstdio>

namespace {

/* The level this machine last followed to, so a round that is announced once
 * is acted on once. Zero means "in the lobby, nothing followed". */
int g_followed = 0;

/* Said once per destination, not once per frame - the retry below runs every
 * frame while the level state is busy, and a refusal is expected during a
 * transition rather than notable. */
int g_said = 0;

}  // namespace

void lsw2_roundfollow_tick() {
    if (!ui_fork_build()) return;

    Lsw2ForkSession& s = lsw2_fork_session();

    /* THE HOST WARPED ITSELF. It is the machine that announced the round, and
     * following its own announcement would be a second warp into the level it
     * is already loading. */
    if (s.is_host()) return;
    if (s.state() == Lsw2SessionState::Offline) return;

    const int want = s.settings().round_level;

    /* BACK TO THE LOBBY IS A STATE CHANGE TOO. When the host clears the field
     * at the end of a round this forgets what it followed, so the next round
     * on the same map is still seen as new. */
    if (want == 0) {
        g_followed = 0;
        return;
    }
    if (want == g_followed) return;
    if (want < 0 || want >= kLsw2LevelCount) return;

    /* ALREADY THERE. A joiner that arrives mid-round adopts the settings and
     * lands here, and if the game has put it in the arena by some other route
     * there is nothing to do but record it. */
    if (lsw2_cheats_current_level() == want) {
        g_followed = want;
        return;
    }

    if (!lsw2_cheats_warp(want)) {
        /* REFUSED, NOT DROPPED. lsw2_cheats_warp declines while the level
         * state is mid-construction, which is precisely when a client that has
         * just been told about a round is likely to ask. Leaving g_followed
         * alone means the next frame tries again. */
        if (g_said != want) {
            g_said = want;
            std::fprintf(stderr,
                         "[fork] round follow: the host is on level %d and the "
                         "warp was refused - the level state is busy, retrying\n",
                         want);
        }
        return;
    }

    g_followed = want;
    std::fprintf(stderr,
                 "[fork] round follow: the host started a round on level %d - "
                 "following\n", want);
}
