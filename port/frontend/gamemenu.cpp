/* See gamemenu.h. Model only - no drawing. */

#include "gamemenu.h"

void Lsw2Menu::reset(const char* title) {
    title_ = title;
    count_ = 0;
    cursor_ = 0;
}

int Lsw2Menu::add(const Lsw2MenuRow& row) {
    if (count_ >= kLsw2MenuMaxRows) return -1;
    rows_[count_] = row;
    /* THE CURSOR MUST START SOMEWHERE VISIBLE. A menu whose first row is
     * hidden - "Character:" under a Roster that is not One - would otherwise
     * open with the cursor on a row nobody can see. */
    const bool cursor_bad = count_ == 0 || rows_[cursor_].hidden ||
                            !rows_[cursor_].selectable;
    if (cursor_bad && !row.hidden && row.selectable) cursor_ = count_;
    return count_++;
}

int Lsw2Menu::next_visible(int from, int direction) const {
    if (count_ == 0) return 0;
    int i = from;
    /* At most one full lap: if every row is hidden, stop rather than spin. */
    for (int guard = 0; guard < count_; ++guard) {
        i += direction;
        if (i < 0) i = count_ - 1;
        if (i >= count_) i = 0;
        if (!rows_[i].hidden && rows_[i].selectable) return i;
    }
    return from;
}

Lsw2MenuEvent Lsw2Menu::step(uint16_t pressed) {
    if (count_ == 0) return Lsw2MenuEvent::None;

    if (pressed & kLsw2KeyB) return Lsw2MenuEvent::Cancelled;

    if (pressed & kLsw2KeyUp) {
        cursor_ = next_visible(cursor_, -1);
        return Lsw2MenuEvent::None;
    }
    if (pressed & kLsw2KeyDown) {
        cursor_ = next_visible(cursor_, +1);
        return Lsw2MenuEvent::None;
    }

    Lsw2MenuRow& r = rows_[cursor_];

    if ((pressed & (kLsw2KeyLeft | kLsw2KeyRight)) && r.value_count > 0) {
        /* WRAPPING IN BOTH DIRECTIONS, a stated requirement of the character
         * customization rows: going only right or only left still cycles
         * through every value, e.g. Left from Bounty Hunters 1 reaches Bounty
         * Hunters 2 and vice versa. The same rule applies to every cycling
         * row, so it lives here rather than in each menu that builds one. */
        const int step = (pressed & kLsw2KeyRight) ? +1 : -1;
        r.value = (r.value + step + r.value_count) % r.value_count;
        return Lsw2MenuEvent::ValueChanged;
    }

    if (pressed & kLsw2KeyA) return Lsw2MenuEvent::Activated;

    return Lsw2MenuEvent::None;
}

const char* Lsw2Menu::value_of(int i) const {
    const Lsw2MenuRow& r = rows_[i];
    if (r.value_count <= 0) return nullptr;
    /* An owner-rendered value wins, and is the ONLY thing consulted when set -
     * a row with thirty-two values has no meaningful `values[value]` to fall
     * back to, and reading one would be an out-of-bounds index. */
    if (r.text_value) return r.text_value;
    if (r.value < 0 || r.value >= kLsw2MenuMaxValues) return nullptr;
    return r.values[r.value];
}

int Lsw2Menu::row_y(int i) const {
    /* Hidden rows do not consume a line, so a menu that hides one closes up
     * instead of leaving a gap where it used to be. */
    int line = 0;
    for (int k = 0; k < i && k < count_; ++k)
        if (!rows_[k].hidden) ++line;
    return kLsw2MenuFirstRowY + line * kLsw2MenuRowPitch;
}

void Lsw2Menu::set_hidden(int i, bool hidden) {
    if (i < 0 || i >= count_) return;
    rows_[i].hidden = hidden;
    /* If the cursor was sitting on the row that just vanished, move it - a
     * cursor on a hidden row would let A activate something invisible. */
    if (hidden && cursor_ == i) cursor_ = next_visible(cursor_, +1);
}

void Lsw2Menu::set_cursor(int i) {
    if (i < 0 || i >= row_count()) return;
    if (rows_[i].hidden) return;
    cursor_ = i;
}
