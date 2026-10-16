/* The host settings' between-match lobby timer.
 *
 * LOBBY TIME is OFF, 30s, 60s or 300s (default 30s): how long the
 * between-match period lasts while 2-4 players are connected.
 *
 * With lobby time OFF, the host starts the match manually with the
 * "Start Game" option of the console entity. Otherwise the countdown starts
 * as soon as a second player is present, and the round follows it.
 *
 * TWO STATEMENTS, FOUR RULES, and the second is the one that is easy to
 * implement wrongly:
 *
 *   1. OFF means no timer at all - "Start Game" is the only way in.
 *   2. The timer starts when a SECOND player joins, not when the lobby opens.
 *      A host sitting alone waits indefinitely, which is what they want.
 *   3. "while 2-4 players are connected" - so it stops if the lobby empties back
 *      to one. A countdown that kept running after everyone left would start a
 *      match for nobody.
 *   4. It belongs to the host. A client counting down independently would race
 *      the host's own count and start on a different frame.
 *
 * THE RULES ARE PURE AND LIVE HERE; the session owns the clock reading and the
 * action. Split for the same reason `customization_policy.cpp` and
 * `scorescreen_policy.cpp` are: these four rules are exactly the part worth
 * pinning, and pinning them must not need a ROM.
 */

#ifndef LSW2_LOBBYTIMER_H
#define LSW2_LOBBYTIMER_H

#include <cstdint>

/* Everything the decision needs, so the decision itself takes no globals. */
struct Lsw2LobbyTimerInput {
    bool hosting = false;      /* this machine owns the countdown       */
    bool in_match = false;     /* a round is running; the lobby is not  */
    int players = 1;           /* including the host                    */
    int lobby_seconds = 30;    /* 0 = OFF                               */
    uint32_t now = 0;          /* the game's seconds counter            */
    /* A joiner is being seated (vwh.h lsw2_vwh_seating): the lobby reloads
     * for every join, so the round must not start under it. HELD, not
     * stopped: a countdown that runs out while held fires on the first tick
     * after the hold lifts. And the timer is still TICKED while held - that
     * is the point of taking it as an input. fork.cpp used to skip the tick
     * instead, so a joiner who waited through a whole match hid
     * the match from the timer: its fired latch outlived the round and the
     * next lobby counted "0 to go" forever. */
    bool hold = false;
};

/* What the timer is doing, so a caller can both act on it and show it. */
struct Lsw2LobbyTimer {
    bool running = false;
    uint32_t started = 0;      /* `now` when it began                   */
    bool fired = false;        /* expired and not yet consumed          */

    /* One tick. Returns true on the single tick where the match should start.
     *
     * IDEMPOTENT AFTER FIRING: it will not return true again until the timer
     * has been stopped and restarted by the rules above, because a second start
     * request while a round is loading is a race, not a retry. */
    bool tick(const Lsw2LobbyTimerInput& in);

    /* Seconds left, or -1 when the timer is not running. For the on-screen
     * countdown that gives the player a number to watch. */
    int remaining(const Lsw2LobbyTimerInput& in) const;

    void reset();
};

#endif /* LSW2_LOBBYTIMER_H */
