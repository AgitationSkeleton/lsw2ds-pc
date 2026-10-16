/* The fork's pause layout - the part a screenshot confirmed, pinned so it stays true.
 *
 * This menu took three attempts and the third could not execute at all. The
 * arithmetic below is what finally produced three evenly spaced, centred rows;
 * these checks exist so a later edit cannot quietly undo it. */

#include "pauselayout.h"

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
    /* --- HIDING BY IDENTITY, which is the rule -----------------------------
     * The ROM builds its rows at runtime and swaps the wireless
     * label, so position is not identity. These are the ids the design names.
     */
    check(lsw2_pause_id_hidden(kLsw2PauseIdHostWireless), "Host Wireless hides by id");
    check(lsw2_pause_id_hidden(kLsw2PauseIdShutOffWireless),
          "Shut Off Wireless hides too - the same row in its other state");
    /* THE STATE THE FORK ACTUALLY RUNS IN. 0x0208E8F4 picks 0x03E when the
     * multi-view bit is set, which is the arena. A hidden set of {0x3C, 0x3D}
     * would show this row in the one place it matters. */
    check(lsw2_pause_id_hidden(kLsw2PauseIdExitWireless),
          "Exit Wireless Game hides - the multi-view label, i.e. the arena's");
    check(lsw2_pause_id_hidden(kLsw2PauseIdExtras), "Extras hides by id");
    check(lsw2_pause_id_hidden(kLsw2PauseIdSaveGame), "Save Game hides by id");
    check(!lsw2_pause_id_hidden(kLsw2PauseIdQuitGame), "Quit Game survives");
    check(!lsw2_pause_id_hidden(kLsw2PauseIdCustomize),
          "Character Customize survives - the fork keeps customization");

    /* --- the fallback, and the handover ------------------------------------ */
    check(!lsw2_pause_row_id_known(0), "no row identity before the builder runs");
    check(lsw2_pause_row_hidden(1) && lsw2_pause_row_hidden(3) &&
              lsw2_pause_row_hidden(4),
          "until then, the screenshot's positions answer");

    /* THE CASE INDEX-HIDING GETS WRONG. A menu where Quit Game happens to sit
     * at index 1 - one row dropped above it - would have Quit hidden and Host
     * Wireless shown. With identities recorded, position stops mattering. */
    lsw2_pause_note_row_id(0, kLsw2PauseIdQuitGame);   /* row 0 clears first */
    lsw2_pause_note_row_id(1, kLsw2PauseIdQuitGame);
    lsw2_pause_note_row_id(2, kLsw2PauseIdHostWireless);
    check(lsw2_pause_row_id_known(1), "the builder's pass records identities");
    check(!lsw2_pause_row_hidden(1), "Quit Game at index 1 is NOT hidden");
    check(lsw2_pause_row_hidden(2), "Host Wireless at index 2 IS hidden");

    /* A fresh pass through row 0 forgets the old shape. */
    lsw2_pause_note_row_id(0, kLsw2PauseIdCustomize);
    check(!lsw2_pause_row_id_known(2),
          "row 0 clears the table so a changed menu is not answered stale");
    check(!lsw2_pause_row_hidden(0), "Character Customize at row 0 survives");

    /* Put the recorded state back the way the rest of the file expects. */
    lsw2_pause_note_row_id(0, 0u);

    /* --- which rows the fork hides ---------------------------------------- */
    check(lsw2_pause_row_hidden(1), "Host Wireless is hidden");
    check(lsw2_pause_row_hidden(3), "Extras is hidden");
    check(lsw2_pause_row_hidden(4), "Save Game is hidden");
    check(!lsw2_pause_row_hidden(0) && !lsw2_pause_row_hidden(2) &&
              !lsw2_pause_row_hidden(5),
          "Resume, Settings and Quit Game survive");

    /* --- THE LAYOUT A SCREENSHOT CONFIRMED --------------------------------
     * 77 / 102 / 127: contiguous at the game's own 25-pixel pitch. Before the
     * fix these were 40 / 90 / 165 - three holes and a menu that read as
     * broken rather than short. */
    check(lsw2_pause_row_y(0) == 77, "Resume sits at 77");
    check(lsw2_pause_row_y(2) == 102, "Settings sits at 102");
    check(lsw2_pause_row_y(5) == 127, "Quit Game sits at 127");

    check(lsw2_pause_row_y(2) - lsw2_pause_row_y(0) == kLsw2PausePitch &&
              lsw2_pause_row_y(5) - lsw2_pause_row_y(2) == kLsw2PausePitch,
          "the survivors are one pitch apart - no holes");

    /* --- and still centred where the art frames it ------------------------
     * Six rows from 40 at 25 span 40..165, centre 102.5. Three rows at
     * 77..127 centre on 102. Half a pixel, which is integer division and not
     * worth a float. */
    check((lsw2_pause_row_y(0) + lsw2_pause_row_y(5)) / 2 == 102,
          "the block keeps the midpoint the full six occupied");

    /* --- hidden rows go OFF THE SCREEN ------------------------------------
     * This used to assert the original slot, with the reason "it draws nothing
     * anyway". It drew. Photographed 2026-09-12 on a hosting fork: "Shut Off
     * Wireless" on top of "Resume", because three visible rows compact into
     * exactly row 1's original slot. A test that encodes the wrong policy is
     * green evidence for a defect. */
    check(lsw2_pause_row_y(1) == kLsw2PauseOffScreenY,
          "a hidden row is placed below the 192-line screen, where the "
          "builder's own draw cannot show it");

    /* --- THE CURSOR DIRECTION, which is the bug that was seen in play -----
     * Down onto Resume always worked: the guest wraps 5 -> 0 and 0 is visible,
     * so no correction happens. Up goes 2 -> 1, which IS hidden. */
    check(lsw2_pause_escape(1, 2) == 0,
          "PRESSING UP out of Settings reaches Resume, not Quit Game");
    check(lsw2_pause_escape(3, 2) == 5,
          "pressing down out of Settings reaches Quit Game");
    check(lsw2_pause_escape(4, 5) == 2,
          "pressing up out of Quit Game skips both hidden rows to Settings");
    check(lsw2_pause_escape(0, 5) == 0,
          "a visible row is never moved");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
