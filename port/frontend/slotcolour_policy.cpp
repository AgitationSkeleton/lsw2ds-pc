/* The pure half of the slot-colour portrait ring - see slotcolour.h. No framework, so the rule is
 * testable without OAM, a session, or a frame. */

#include "slotcolour.h"

int lsw2_slot_palette(int slot) {
    if (slot < 0 || slot >= kLsw2SlotPalettes) return -1;
    return kLsw2FirstSlotPalette + slot;
}

int lsw2_slotcolour_rewrite(int palette, int local_slot) {
    const int want = lsw2_slot_palette(local_slot);
    if (want < 0) return -1;

    /* SLOT 0 IS ALREADY RIGHT. The game draws the ring blue, which is slot 1's
     * colour, so a host - or anyone offline - needs nothing done. Returning -1
     * rather than writing the same value keeps the log honest: it reports what
     * it CHANGED, and a host would otherwise see a line every frame saying a
     * palette had been set to what it already was. */
    if (want == kLsw2FirstSlotPalette) return -1;

    /* ONLY THE PLAYER-COLOUR RANGE. Everything else on the screen is out of
     * scope by construction - no position test to drift, no size test to go
     * stale when a sprite is resized. */
    if (palette < kLsw2FirstSlotPalette ||
        palette >= kLsw2FirstSlotPalette + kLsw2SlotPalettes)
        return -1;

    if (palette == want) return -1;   /* already correct */
    return want;
}
