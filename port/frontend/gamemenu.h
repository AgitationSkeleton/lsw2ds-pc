/* Menus in the game's own presentation, for the fork.
 *
 * WHY THIS EXISTS. The fork's design puts almost every fork
 * control **in the game** rather than in imgui - the menu the host configures
 * the match with, the one the lobby console opens, the Extras submenu under it,
 * and the roster picker. imgui is left with
 * only the things that have no sensible home inside the game - the player name
 * and the master server address. So the port needs to draw a menu that looks like LSW2's, and
 * that is this.
 *
 * MODEL AND RENDERER ARE SEPARATE, deliberately. There are two ways
 * to get pixels on screen:
 *
 *   B1  drive the game's own menu widget, so the look is the game's by
 *       construction. Blocked on the row array's owner (four mechanisms
 *       refuted so far), which is one read of a running game away.
 *   B2  draw through the compositor using the game's own font glyphs, which
 *       tools/export_custom_fonts.py already extracts.
 *
 * Everything in this header is the MODEL - rows, values, cursor, input, the
 * callbacks. It contains no drawing at all. That means B1 and B2 are a choice of
 * backend rather than a rewrite, and the decision can wait for evidence instead
 * of being guessed now.
 *
 * THE GEOMETRY IS THE GAME'S, measured rather than chosen: reading the
 * pause menu's own rows out of RAM and found y stepping by exactly 25, every row
 * centred on x = 128, and a row height of 18. A menu drawn on those numbers sits
 * where the game's menus sit.
 */

#ifndef LSW2_GAMEMENU_H
#define LSW2_GAMEMENU_H

#include <cstdint>

/* THE ARENA'S OWN METRICS, not the pause menu's.
 *
 * These were 40 and 25, measured from a real PAUSE menu. The cartridge's own
 * arena menu, at 0x0209CADC, is laid out differently:
 *
 *     0209CB00  MOV r4, #0x1C           the row pitch, 28
 *     0209CB68  MOV r2, #0x80           X = 128, centred
 *     0209CB6C  ADD r3, r3, #0x3A       first row Y = 58
 *     0209CB80  CMP r9, #4              four rows
 *
 * The fork's menus are shown IN THE ARENA, and anything the fork adds in-game
 * has to look like it shipped with the unmodified game. For a menu
 * that appears in the arena, the arena's metrics are that style. Some of the
 * text on the multiplayer menu was seen to be misaligned; this is the measured
 * candidate for part of it.
 *
 * Finding the arena menu's metrics did not by itself change these, because
 * which menus should follow which screen is a presentation judgement rather
 * than a reading. The judgement made: ALL of the fork's menus take the arena's
 * metrics.
 *
 * X and the row height are unchanged - both menus centre on 128, and the row
 * height was never in question. */
constexpr int kLsw2MenuFirstRowY = 58;   /* ADD r3, r3, #0x3A at 0x0209CB6C */
constexpr int kLsw2MenuRowPitch = 28;    /* MOV r4, #0x1C     at 0x0209CB00 */
constexpr int kLsw2MenuRowHeight = 18;
constexpr int kLsw2MenuCentreX = 128;

constexpr int kLsw2MenuMaxRows = 12;
constexpr int kLsw2MenuMaxValues = 6;

/* A row is a label, and optionally a value the player cycles with left/right.
 *
 * `values` being null makes the row an action - "Start Lobby", "Return" - which
 * A activates. Otherwise A opens whatever deeper thing the row owns, which is
 * how the host menu's "Extras:" and "Character:" rows behave. */
struct Lsw2MenuRow {
    const char* label = nullptr;
    const char* values[kLsw2MenuMaxValues] = {nullptr};
    int value_count = 0;
    int value = 0;

    /* A VALUE THE ROW'S OWNER RENDERS ITSELF.
     *
     * `values` holds at most six, which is right for a toggle or a short list
     * and useless for anything chosen from a large set - a room code character
     * comes from thirty-two, and a character picker from the whole roster.
     *
     * When this is set it is what the row displays, and `values` is ignored.
     * `value` still cycles over `value_count` exactly as before, so left and
     * right behave identically; the row's owner just decides what each index looks
     * like and points this at its own storage. That storage must outlive the
     * menu, which is the same contract the labels already have. */
    const char* text_value = nullptr;
    /* Hidden rows keep their place in the table but are skipped by the cursor
     * and not drawn. The host menu needs exactly this: choosing One reveals
     * an extra row beneath it, and moving off One hides that row again and
     * disregards its value. */
    bool hidden = false;
    /* Drawn, but the cursor passes over it.
     *
     * Distinct from `hidden` because a menu can contain information as well as
     * choices - the Bounty Hunter lobby console shows the room code as a line
     * of its own, and a cursor able to stop on it is a dead end the player has to
     * press past. Hiding it would remove the information; this keeps it and
     * removes only the stop. */
    bool selectable = true;

    /* A SMALL ICON DRAWN AFTER THE ROW'S TEXT.
     *
     * A master server list row holds the name, the seats taken of 4, the ping
     * in milliseconds, and a DS signal-strength icon. The first three are text; the fourth is not,
     * and it is the reason `Lsw2MasterRow::signal` sat computed and unread -
     * the menu had no way to say "and then some bars".
     *
     * An int rather than an enum because gamemenu.h is the generic widget and
     * must not learn what a master server is: 0 draws nothing, and 1..3 draw
     * that many bars. A future badge that is not bars gets its own range rather
     * than overloading these. */
    int badge = 0;

    /* PER-ROW COLOUR, or 0 for the menu's own.
     *
     * The master server list shows LAN games highlighted in green text at the
     * top of the list. Until now every row drew in one of two colours -
     * selected or not - so a row could not say anything about itself.
     *
     * ZERO MEANS "THE MENU DECIDES", not black. A row that wants black is not a
     * thing any menu asks for, and treating 0 as a colour would make every
     * default-constructed row invisible, which is the kind of failure that looks
     * like the menu not drawing at all. The cursor still wins: a highlighted row
     * under the cursor draws as selected, because losing the cursor to make a
     * row green would be trading a control for a decoration. */
    uint32_t colour = 0;
};

/* The green the master server list uses for LAN games. Bright enough to read over the game's own
 * frame, which is why it is not 0x00FF00 - pure green on a light background is
 * the one colour that disappears. */
constexpr uint32_t kLsw2MenuGreen = 0x40E060u;

/* What the player just did, returned by step() so the caller can act without
 * this module knowing anything about lobbies or networking. */
enum class Lsw2MenuEvent {
    None,
    ValueChanged,   /* left/right on the cursor row */
    Activated,      /* A on the cursor row          */
    Cancelled,      /* B                            */
};

class Lsw2Menu {
public:
    void reset(const char* title);
    /* Returns the row's index, or -1 when the menu is full. */
    int add(const Lsw2MenuRow& row);

    /* One frame of input. `pressed` is a DS key mask in KEYINPUT bit order, and
     * only newly-pressed bits should be passed - this does no edge detection of
     * its own, because the caller already has the previous mask. */
    Lsw2MenuEvent step(uint16_t pressed);

    int cursor() const { return cursor_; }

    /* PUT THE CURSOR ON A ROW DIRECTLY, for a touch.
     *
     * The d-pad moves the cursor one step at a time and skips hidden rows on
     * the way; a tap names a row outright. Refuses a hidden or out-of-range
     * row rather than clamping to a neighbour - a tap that misses should do
     * nothing, not activate whatever was nearest. */
    void set_cursor(int i);
    int row_count() const { return count_; }
    const Lsw2MenuRow& row(int i) const { return rows_[i]; }
    Lsw2MenuRow& row(int i) { return rows_[i]; }
    const char* title() const { return title_; }

    /* The value a row currently shows, or null for an action row. */
    const char* value_of(int i) const;

    /* Where a row is drawn, in DS pixels, using the game's own geometry. Rows
     * that are hidden do not consume a line, so the menu closes up rather than
     * leaving a gap - which is what the host menu's re-hiding requires. */
    int row_y(int i) const;

    void set_hidden(int i, bool hidden);

private:
    int next_visible(int from, int direction) const;

    const char* title_ = nullptr;
    Lsw2MenuRow rows_[kLsw2MenuMaxRows];
    int count_ = 0;
    int cursor_ = 0;
};

/* DS key bits, KEYINPUT order, so callers do not restate them. */
constexpr uint16_t kLsw2KeyA = 1u << 0;
constexpr uint16_t kLsw2KeyB = 1u << 1;
constexpr uint16_t kLsw2KeyRight = 1u << 4;
constexpr uint16_t kLsw2KeyLeft = 1u << 5;
constexpr uint16_t kLsw2KeyUp = 1u << 6;
constexpr uint16_t kLsw2KeyDown = 1u << 7;

#endif /* LSW2_GAMEMENU_H */
