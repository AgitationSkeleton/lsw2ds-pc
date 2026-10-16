/* The rule that would have broken every match - see lobbypin.h. */

#include "lobbypin.h"

#include <cstdio>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok: %s\n", what);
    } else {
        ++g_failed;
        std::printf("  FAIL: %s\n", what);
    }
}

int main() {
    const int lobby = 9;

    /* --- THE REGRESSION THIS FILE EXISTS FOR ------------------------------
     * The backstop warped back from any level that was not the lobby, and the
     * arena is not the lobby. Every round would have been dragged out of. */
    check(!lsw2_lobbypin_should_warp_back(4, lobby, /*is_arena=*/true, false),
          "AN ARENA IS NEVER WARPED OUT OF - the match happens there");
    check(!lsw2_lobbypin_should_warp_back(5, lobby, true, false),
          "...and that holds for the second arena map too");

    /* --- the door, which is what the backstop is for ---------------------- */
    check(lsw2_lobbypin_should_warp_back(2, lobby, false, false),
          "a non-arena level that is not the lobby IS warped back - the door");

    /* --- already home ----------------------------------------------------- */
    check(!lsw2_lobbypin_should_warp_back(lobby, lobby, false, false),
          "the lobby itself is left alone");

    /* --- the cooldown ------------------------------------------------------
     * Without it the fork posts a request every frame into a state machine
     * already building one, which is the documented way to crash the guest. */
    check(!lsw2_lobbypin_should_warp_back(2, lobby, false, /*cooling=*/true),
          "nothing is requested while the previous warp is still in flight");

    /* --- quit versus door -------------------------------------------------
     * Both land outside the lobby. Only one should drop the session. */
    check(lsw2_lobbypin_is_quit(-1, true, /*match_running=*/false),
          "off-level while connected is Quit Game - disconnect");

    /* --- THE REGRESSION THAT ENDED HALF OF ALL MATCHES --------------------
     * The arena transition is off-level for about a second and a half while
     * the level loads, and this predicate runs every frame. Treating that as a
     * quit shut the session down mid-match:
     *     [fork] JOIN GAP (HOST) state 41 level -1
     *     [fork] left the lobby while connected - disconnecting
     * It is a race against a 1.4 s window, so it struck in roughly half of
     * otherwise identical runs - and was first blamed on an unrelated change. */
    check(!lsw2_lobbypin_is_quit(-1, true, /*match_running=*/true),
          "OFF-LEVEL WHILE THE MATCH RUNS IS THE ARENA LOADING - never a quit");
    check(!lsw2_lobbypin_is_quit(2, true, true),
          "a valid level during a match is not a quit either");

    /* --- THE GAP A LEVEL LOAD OPENS --------------------------------------
     * The first fix tested the CURRENT state and still disconnected, because
     * the watchdog fires while the state is -1 - the trail is 41 -> -1 -> 42
     * and the shutdown landed in the middle. */
    check(lsw2_lobbypin_settle_state(-1, 41) == 41,
          "a -1 state carries the last real state across a level load");
    check(lsw2_lobbypin_settle_state(42, 41) == 42,
          "a real state replaces it");
    check(lsw2_lobbypin_settle_state(-1, 48) == 48,
          "and the lobby is carried across its own gaps too, so a genuine "
          "quit from the lobby still disconnects");

    /* --- THE ROUND START, WHICH THE CARRIED STATE CANNOT SEE --------------
     * Measured on four consoles: Game Settings' Start posts the lobby level to
     * the world slot and 41 to the screen slot. The world unloads while the
     * screen still reads 54 on the host and 48 on a joiner:
     *     [fork] JOIN GAP (HOST) state 54 level -1
     *     [fork] disconnected through the pause menu (shut down the lobby)
     * Only the pending request knows 41 is next. */
    check(lsw2_lobbypin_match_bound(54, 41),
          "GAME SETTINGS WITH 41 PENDING IS THE ROUND START - never a quit");
    check(lsw2_lobbypin_match_bound(48, 41),
          "...and so is the lobby with 41 pending, which is what a joiner sees");
    check(lsw2_lobbypin_match_bound(41, -1) && lsw2_lobbypin_match_bound(-1, 42),
          "either end of the transition naming the match is enough");
    check(!lsw2_lobbypin_match_bound(48, -1),
          "the lobby with nothing pending is not bound for a match");
    check(!lsw2_lobbypin_match_bound(48, 15),
          "Quit Game's own screen request (0x0F) is not the match");

    check(!lsw2_lobbypin_is_quit(2, true, false),
          "A VALID LEVEL IS THE DOOR - walking through one must NOT disconnect "
          "a host");
    check(!lsw2_lobbypin_is_quit(-1, false, false),
          "off-level with no session is just the front end, nothing to drop");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
