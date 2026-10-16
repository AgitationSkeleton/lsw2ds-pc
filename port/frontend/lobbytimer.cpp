/* See lobbytimer.h. */

#include "lobbytimer.h"

namespace {

/* Whether the conditions for a countdown are met right now.
 *
 * "While 2-4 players are connected" is read as ">= 2" rather than "2..4"
 * exactly: the transport refuses a fifth station (lsw2_lockstep.cpp), so four is
 * already the ceiling and re-testing it here would be a second place to keep in
 * step with the arena's width. */
bool wants_countdown(const Lsw2LobbyTimerInput& in) {
    return in.hosting && !in.in_match && in.lobby_seconds > 0 && in.players >= 2;
}

}  // namespace

void Lsw2LobbyTimer::reset() {
    running = false;
    started = 0;
    fired = false;
}

bool Lsw2LobbyTimer::tick(const Lsw2LobbyTimerInput& in) {
    if (!wants_countdown(in)) {
        /* STOPPED, AND THE FIRED LATCH GOES WITH IT. The next time two players
         * are in the lobby together it is a new between-match period and gets a
         * whole new countdown - when a match is over, an enabled lobby timer
         * restarts from the top. */
        reset();
        return false;
    }

    if (!running) {
        running = true;
        started = in.now;
        fired = false;
        return false;
    }

    if (fired) return false;

    /* HELD while a joiner is seated: the count goes on, the start waits. */
    if (in.hold) return false;

    /* UNSIGNED SUBTRACTION. The counter is a free-running word, and
     * `now >= started + seconds` would never become true on the one countdown
     * that straddles a wrap - leaving a lobby that never starts, once, on a
     * machine that had been running a long time. The same rule scorescreen.cpp
     * uses, for the same clock. */
    if ((in.now - started) < uint32_t(in.lobby_seconds)) return false;

    fired = true;
    return true;
}

int Lsw2LobbyTimer::remaining(const Lsw2LobbyTimerInput& in) const {
    if (!running) return -1;
    const uint32_t gone = in.now - started;
    if (gone >= uint32_t(in.lobby_seconds)) return 0;
    return in.lobby_seconds - int(gone);
}
