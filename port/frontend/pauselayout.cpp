/* See pauselayout.h. Pure - no guest, no framework. */

#include "pauselayout.h"

namespace {

/* THE FALLBACK, used until the builder has been round a row and told us what it
 * actually is. These are the positions the confirming screenshot showed.
 *
 * AND THEY ARE SOUNDER THAN FIRST FEARED. An earlier reading
 * warned the row SET might vary, making index hiding unsafe. It does not: the
 * selection handler `0x0208EA58` bounds the cursor at 5 and jump-tables six
 * entries, so a menu with a different number of rows would dispatch wrongly
 * through the game's own table. The count is six.
 *
 * What varies is row 1's LABEL - 0x03C "Host Wireless", 0x03D "Shut Off
 * Wireless", 0x03E "Exit Wireless Game" - and that is the reason the id rule
 * below is still the right one: it is what the hidden-row rule names, and it survives
 * the swap without the port having to know which label is showing. */
/* See pauselayout.h - kLsw2PauseHiddenRows is the one copy. */

/* Row -> string id, filled by lsw2_pause_note_row_id as the builder draws.
 * 0 is not a valid menu string here, so it doubles as "not seen yet". */
uint32_t g_row_id[kLsw2PauseRowCount] = {0, 0, 0, 0, 0, 0};

}  // namespace

bool lsw2_pause_id_hidden(uint32_t string_id) {
    /* ALL THREE WIRELESS LABELS. 0x0208E874 swaps 0x03C for 0x03D when a session is
     * up and 0x03E "Exit Wireless Game" when the multi-view bit is set, and
     * "hide Host Wireless" plainly means that row in ALL THREE states -
     * offering "Shut Off Wireless" in a fork that never hosts one would be
     * worse than offering nothing. */
    return string_id == kLsw2PauseIdHostWireless ||
           string_id == kLsw2PauseIdShutOffWireless ||
           string_id == kLsw2PauseIdExitWireless ||
           string_id == kLsw2PauseIdExtras ||
           string_id == kLsw2PauseIdSaveGame;
}

void lsw2_pause_note_row_id(uint32_t row, uint32_t string_id) {
    if (row >= kLsw2PauseRowCount) return;

    /* ROW 0 CLEARS. The loop restarts at row 0 every frame, so a menu that
     * gained or lost a row must not answer from the previous shape. Clearing
     * on the way past row 0 keeps the table exactly as fresh as the frame. */
    if (row == 0) {
        for (uint32_t i = 0; i < kLsw2PauseRowCount; ++i) g_row_id[i] = 0;
    }
    g_row_id[row] = string_id;
}

bool lsw2_pause_row_id_known(uint32_t row) {
    return row < kLsw2PauseRowCount && g_row_id[row] != 0;
}

bool lsw2_pause_row_hidden(uint32_t row) {
    /* IDENTITY WHEN WE HAVE IT. Once the builder has been round this row we
     * know what it says, and what it says is what the hidden-row rule names. */
    if (lsw2_pause_row_id_known(row)) return lsw2_pause_id_hidden(g_row_id[row]);

    /* POSITION UNTIL THEN. The cursor and layout helpers are asked before the
     * first draw of a freshly opened menu, and answering "nothing is hidden"
     * there would flash all six rows for a frame. The screenshot's positions
     * are the best available answer for exactly that one frame. */
    for (uint32_t h : kLsw2PauseHiddenRows)
        if (h == row) return true;
    return false;
}

uint32_t lsw2_pause_row_y(uint32_t row) {
    /* A HIDDEN ROW GOES OFF THE SCREEN, not back to its original slot.
     *
     * PHOTOGRAPHED, the first time the trimmed pause menu was ever looked at
     * rather than read: a hosting fork's pause menu drew "Shut Off Wireless" -
     * the wireless row, which the fork hides - and drew it ON TOP of "Resume". This function was
     * why. It handed a hidden row its ORIGINAL y and compacted the visible rows
     * around it, and with three visible rows Resume's compacted centre is
     * exactly hidden row 1's original slot. The row was disabled for the cursor
     * and never for the draw; "hidden" meant "unselectable and re-laid", and
     * the game drew it where it always had.
     *
     * The bottom screen is 192 lines. A row placed below that is submitted and
     * never rasterised, which is the nearest thing to "not drawn" the builder's
     * own per-row Y offers - and R7 at the hook IS the builder's decision
     * about where this row goes, taken every time round its loop. Changing the
     * value handed back at that point is changing the decision, not covering
     * the result; the previous value was already ours. */
    if (lsw2_pause_row_hidden(row)) return kLsw2PauseOffScreenY;

    uint32_t visible = 0;
    for (uint32_t r = 0; r < kLsw2PauseRowCount; ++r)
        if (!lsw2_pause_row_hidden(r)) ++visible;

    uint32_t ordinal = 0;
    for (uint32_t r = 0; r < row; ++r)
        if (!lsw2_pause_row_hidden(r)) ++ordinal;

    /* Six rows at 25 from 40 have their centre at 40 + 25*(6-1)/2; three want to
     * start half the difference lower. Integer arithmetic throughout - the half
     * pixel it loses is not visible and a float here would be theatre. */
    const uint32_t top = kLsw2PauseFirstY +
                         kLsw2PausePitch * (kLsw2PauseRowCount - visible) / 2u;
    return top + kLsw2PausePitch * ordinal;
}

uint32_t lsw2_pause_escape(uint32_t from, uint32_t last) {
    if (from >= kLsw2PauseRowCount || !lsw2_pause_row_hidden(from)) return from;

    const bool backward = from < last;
    uint32_t next = from;
    for (uint32_t step = 0; step < kLsw2PauseRowCount; ++step) {
        next = backward
                   ? (next + kLsw2PauseRowCount - 1u) % kLsw2PauseRowCount
                   : (next + 1u) % kLsw2PauseRowCount;
        if (!lsw2_pause_row_hidden(next)) break;
    }
    return next;
}
