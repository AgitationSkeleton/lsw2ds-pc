/* The between-match lobby timer, asserted against the two statements of its
 * rules in lobbytimer.h.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_lobbytimer.cpp \
 *       port/frontend/lobbytimer.cpp -o test_lobbytimer && ./test_lobbytimer
 */

#include "lobbytimer.h"

#include <cstdio>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

Lsw2LobbyTimerInput lobby(int players, uint32_t now, int seconds = 30) {
    Lsw2LobbyTimerInput in;
    in.hosting = true;
    in.in_match = false;
    in.players = players;
    in.lobby_seconds = seconds;
    in.now = now;
    return in;
}

}  // namespace

int main() {
    std::printf("lobby timer\n");

    /* --- the countdown starts when a SECOND player arrives ----------------- */
    {
        Lsw2LobbyTimer t;
        check(!t.tick(lobby(1, 100)), "a host alone does not start a countdown");
        check(t.remaining(lobby(1, 100)) == -1, "and has nothing to show");

        check(!t.tick(lobby(2, 100)), "a second player starts it");
        check(t.remaining(lobby(2, 100)) == 30, "thirty seconds to go");
        check(t.remaining(lobby(2, 110)) == 20, "counting down");
        check(!t.tick(lobby(2, 129)), "not at twenty-nine");
        check(t.tick(lobby(2, 130)), "fires at thirty");
        check(!t.tick(lobby(2, 131)),
              "and ONCE - a second start request races the round that is loading");
    }

    /* --- lobby time OFF: only the host's manual start begins a round ------- */
    {
        Lsw2LobbyTimer t;
        check(!t.tick(lobby(4, 100, 0)), "OFF never starts");
        check(!t.tick(lobby(4, 100000, 0)), "however long anyone waits");
        check(t.remaining(lobby(4, 100000, 0)) == -1, "and shows no number");
    }

    /* --- "while 2-4 players are connected" -------------------------------- */
    {
        Lsw2LobbyTimer t;
        t.tick(lobby(2, 100));
        check(t.remaining(lobby(2, 120)) == 10, "ten to go");
        check(!t.tick(lobby(1, 121)), "the other player leaves - no start");
        check(t.remaining(lobby(1, 121)) == -1, "the countdown is gone");
        /* AND IT DOES NOT RESUME WHERE IT STOPPED. Someone rejoining gets the
         * full between-match period, not the two seconds left over from a
         * lobby that emptied. */
        check(!t.tick(lobby(2, 122)), "someone rejoins: a NEW countdown");
        check(t.remaining(lobby(2, 122)) == 30, "starting from thirty again");
    }

    /* --- the host owns it -------------------------------------------------- */
    {
        Lsw2LobbyTimer t;
        Lsw2LobbyTimerInput in = lobby(3, 100);
        in.hosting = false;
        check(!t.tick(in), "a client never counts down");
        in.now = 100000;
        check(!t.tick(in), "no matter how long it waits");
    }

    /* --- a match is not the lobby ------------------------------------------ */
    {
        Lsw2LobbyTimer t;
        t.tick(lobby(3, 100));
        Lsw2LobbyTimerInput mid = lobby(3, 105);
        mid.in_match = true;
        check(!t.tick(mid), "the round starts, so the countdown stops");
        /* Everyone back in the lobby restarts an enabled lobby timer - the
         * match ending is what re-arms it. */
        check(!t.tick(lobby(3, 200)), "back in the lobby: a fresh countdown");
        check(t.remaining(lobby(3, 200)) == 30, "beginning anew, from thirty");
        check(t.tick(lobby(3, 230)), "and it fires again");
    }

    /* --- a joiner being seated holds the start, and never hides a match ---- */
    {
        Lsw2LobbyTimer t;
        t.tick(lobby(2, 100));
        Lsw2LobbyTimerInput held = lobby(2, 140);
        held.hold = true;
        check(!t.tick(held), "the countdown ran out while a joiner is seated: held");
        check(t.remaining(held) == 0, "and still shows it has run out");
        check(t.tick(lobby(3, 142)), "the seat is done: the round starts at once");

        /* A joiner who waits through a WHOLE match holds the timer
         * from the lobby, through the round, to the next lobby. The match must
         * still re-arm it. */
        Lsw2LobbyTimer u;
        u.tick(lobby(2, 100));
        check(u.tick(lobby(2, 130)), "round one starts");
        Lsw2LobbyTimerInput mid = lobby(2, 150);
        mid.in_match = true;
        mid.hold = true;
        check(!u.tick(mid), "a joiner waits through the match");
        Lsw2LobbyTimerInput back = lobby(2, 300);
        back.hold = true;
        check(!u.tick(back), "back in the lobby, the joiner is being seated");
        check(u.remaining(back) == 30, "and the countdown began anew - not 0 to go");
        check(u.tick(lobby(3, 330)), "round two starts, with the joiner in it");
    }

    /* --- the free-running counter wraps ------------------------------------ */
    {
        Lsw2LobbyTimer t;
        t.tick(lobby(2, 0xFFFFFFF0u));
        check(t.tick(lobby(2, 0x0000000Eu)),
              "a countdown straddling the wrap still fires");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
