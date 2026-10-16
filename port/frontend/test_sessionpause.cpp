/* The online pause's two rules, asserted: every console updates the paused mask
 * identically, and only the right console acts on each pause-menu message.
 *
 *   g++ -O2 -std=c++20 -Iport/frontend port/frontend/test_sessionpause.cpp \
 *       port/frontend/sessionpause_policy.cpp -o test_sessionpause
 */

#include "sessionpause_policy.h"

#include <cstdio>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

using A = Lsw2PauseMsgAction;

}  // namespace

int main() {
    std::printf("session pause\n");

    /* --- one player's Start: their menu only --------------------------------- */
    {
        uint8_t on_c2 = 0, on_host = 0, on_c1 = 0;
        check(lsw2_pause_message_policy(kLsw2MsgPauseOpen, 2, 2, false, &on_c2) ==
                  A::OpenLocal,
              "station 2's own console opens its menu");
        check(lsw2_pause_message_policy(kLsw2MsgPauseOpen, 2, 0, false, &on_host) ==
                  A::Drop,
              "the host does not open one");
        check(lsw2_pause_message_policy(kLsw2MsgPauseOpen, 2, 1, false, &on_c1) ==
                  A::Drop,
              "nor does another client");
        check(on_c2 == 0x4 && on_host == 0x4 && on_c1 == 0x4,
              "and every console marks station 2 paused, the same way");
        check(lsw2_pause_message_policy(kLsw2MsgPauseOpen, 2, 2, true, &on_c2) ==
                  A::Drop,
              "a second Start with the menu already up opens nothing more");
    }

    /* --- Resume: the sender's menu closes, nobody else's changes ------------ */
    {
        uint8_t on_c2 = 0x4, on_host = 0x4;
        check(lsw2_pause_message_policy(kLsw2MsgPauseResume, 2, 2, true, &on_c2) ==
                  A::Arm,
              "station 2's Resume runs the arm on its own console");
        check(lsw2_pause_message_policy(kLsw2MsgPauseResume, 2, 0, false, &on_host) ==
                  A::Drop,
              "and is dropped elsewhere - the arm would ask for a state nobody "
              "replaced");
        check(on_c2 == 0 && on_host == 0, "every console unmarks station 2");

        uint8_t closed = 0x4;
        check(lsw2_pause_message_policy(kLsw2MsgPauseResume, 2, 2, false, &closed) ==
                  A::Drop,
              "a Resume after a transition already closed the menu is dropped - "
              "the arm would reopen it");
        check(closed == 0, "but still unmarks the station");
    }

    /* --- Quit: a client's is theirs, the host's is everyone's ---------------- */
    {
        uint8_t mask = 0x6;
        check(lsw2_pause_message_policy(kLsw2MsgPauseQuit, 1, 1, true, &mask) == A::Arm,
              "the leaving client's own console quits");
        uint8_t other = 0x6;
        check(lsw2_pause_message_policy(kLsw2MsgPauseQuit, 1, 0, false, &other) ==
                  A::Drop,
              "the host drops a client's quit - it counts the player out");
        check(mask == 0x4 && other == 0x4, "and both unmark the leaver");
        uint8_t client = 0x1;
        check(lsw2_pause_message_policy(kLsw2MsgPauseQuit, 0, 2, false, &client) ==
                  A::Arm,
              "the host's Shut Down Lobby runs on every client");
    }

    /* --- everything else is the cartridge's --------------------------------- */
    {
        uint8_t mask = 0x2;
        check(lsw2_pause_message_policy(0x13, 1, 0, false, &mask) == A::Arm &&
                  mask == 0x2,
              "a count-out message is untouched and changes no mask");
        check(lsw2_pause_message_policy(kLsw2MsgPauseOpen, 7, 0, false, &mask) ==
                  A::Arm,
              "a station outside 0..3 is left to the cartridge");
    }

    /* --- the first frame after a pause --------------------------------------- */
    {
        const Lsw2PadState s = lsw2_pause_resume_edges(0x0003, 0x0001);
        check(s.prev == 0x0001, "previous is the last paused frame's keys, not 0");
        check(s.pressed == 0x0002, "only a key newly down is a press");
        check(s.released == 0, "nothing was released");
        const Lsw2PadState held = lsw2_pause_resume_edges(0x0001, 0x0001);
        check(held.pressed == 0,
              "A still held from the Resume press is not a fresh press");
        const Lsw2PadState up = lsw2_pause_resume_edges(0x0000, 0x0001);
        check(up.released == 0x0001, "letting go is a release");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
