/* See objsprite.h. */

#include "objsprite.h"

namespace {

uint16_t read16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

}  // namespace

void lsw2_obj_dimensions(int shape, int size, int* width, int* height) {
    /* THE HARDWARE TABLE, not a formula.
     *
     * Shape and size are two bits each and the mapping between them and the
     * pixel dimensions is arbitrary - there is no arithmetic that produces it,
     * which is exactly why it is worth writing out and testing rather than
     * deriving. Rows are shape 0 square, 1 wide, 2 tall; columns are size 0..3.
     * Shape 3 is reserved and yields nothing. */
    static const int kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const int kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

    if (width) *width = 0;
    if (height) *height = 0;
    if (shape < 0 || shape > 2 || size < 0 || size > 3) return;
    if (width) *width = kW[shape][size];
    if (height) *height = kH[shape][size];
}

bool lsw2_obj_decode(const uint8_t* oam, int index, Lsw2ObjSprite* out) {
    if (!oam || !out || index < 0 || index >= kLsw2OamEntries) return false;

    const uint8_t* e = oam + index * kLsw2OamEntryBytes;
    const uint16_t a0 = read16(e + 0);
    const uint16_t a1 = read16(e + 2);
    const uint16_t a2 = read16(e + 4);

    Lsw2ObjSprite s;

    /* ATTRIBUTE 0. Bit 8 selects between two meanings for bit 9, and getting
     * that pair backwards is the classic OAM mistake: with affine SET, bit 9 is
     * "double size"; with it CLEAR, bit 9 is "disabled". Reading bit 9 as
     * disabled unconditionally hides every double-sized rotating sprite. */
    s.affine = (a0 & 0x0100u) != 0;
    s.disabled = !s.affine && (a0 & 0x0200u) != 0;
    s.colour256 = (a0 & 0x2000u) != 0;
    const int shape = int((a0 >> 14) & 3u);

    /* Y IS EIGHT BITS AND WRAPS. A sprite at y=200 on a 192-line screen is
     * partly off the bottom, not at a negative position - the hardware has no
     * sign here, unlike x. */
    s.y = int(a0 & 0x00FFu);

    /* ATTRIBUTE 1. X is NINE bits and IS signed, so a sprite hanging off the
     * left edge reads as a large positive number unless it is sign-extended.
     * Treating it as unsigned puts it off the right edge instead - visible, and
     * in the wrong place. */
    int x = int(a1 & 0x01FFu);
    if (x & 0x0100) x -= 0x0200;
    s.x = x;
    const int size = int((a1 >> 14) & 3u);

    lsw2_obj_dimensions(shape, size, &s.width, &s.height);

    /* ATTRIBUTE 2. The palette field is meaningless in 256-colour mode, so it
     * is zeroed rather than passed through - a caller that indexed a palette
     * bank with it would be reading the wrong sixteen entries and would have no
     * way to tell. */
    s.tile = int(a2 & 0x03FFu);
    s.priority = int((a2 >> 10) & 3u);
    s.palette = s.colour256 ? 0 : int((a2 >> 12) & 0x0Fu);

    /* A shape of 3 is reserved; the dimension table gives zero, and a
     * zero-sized sprite is not drawable whatever its other bits say. */
    if (s.width == 0 || s.height == 0) s.disabled = true;

    *out = s;
    return true;
}
