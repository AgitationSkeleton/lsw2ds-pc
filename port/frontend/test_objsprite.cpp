/* The OAM decode, pinned against the hardware's own field layout.
 *
 *   g++ -std=c++20 port/frontend/test_objsprite.cpp \
 *       port/frontend/objsprite.cpp -o test_objsprite
 *
 * WHY THIS IS WORTH A TEST FILE. Every field is a bitfield at a hand-counted
 * offset, and a wrong one does not fail - it draws the wrong tile at the wrong
 * place in the wrong colours. An earlier LDRH mask in this project took three
 * attempts and returned an empty set twice while looking exactly like "there is
 * nothing there".
 *
 * The two traps below are the ones that actually catch people, and both are
 * asserted directly: x is NINE bits and signed, and bit 9 of attribute 0 means
 * two different things depending on bit 8.
 */

#include "objsprite.h"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

uint8_t g_oam[kLsw2OamEntries * kLsw2OamEntryBytes];

void put(int index, uint16_t a0, uint16_t a1, uint16_t a2) {
    uint8_t* e = g_oam + index * kLsw2OamEntryBytes;
    e[0] = uint8_t(a0); e[1] = uint8_t(a0 >> 8);
    e[2] = uint8_t(a1); e[3] = uint8_t(a1 >> 8);
    e[4] = uint8_t(a2); e[5] = uint8_t(a2 >> 8);
    e[6] = 0; e[7] = 0;
}

}  // namespace

int main() {
    std::printf("OAM decode\n");
    std::memset(g_oam, 0, sizeof g_oam);

    /* --- the dimension table -------------------------------------------- */
    int w = 0, h = 0;
    lsw2_obj_dimensions(0, 2, &w, &h);
    check(w == 32 && h == 32, "square size 2 is 32x32");
    lsw2_obj_dimensions(1, 1, &w, &h);
    check(w == 32 && h == 8, "wide size 1 is 32x8");
    lsw2_obj_dimensions(2, 1, &w, &h);
    check(w == 8 && h == 32, "tall size 1 is 8x32");
    lsw2_obj_dimensions(3, 0, &w, &h);
    check(w == 0 && h == 0, "the reserved shape has no size");

    /* Those three are exactly the shapes the HUD's player slot is built from -
     * "a 32x32 body, an 8x32 strip on its right, and a 32x8 strip below it". */

    /* --- a plain slot sprite --------------------------------------------- */
    /* y=5, square 32x32, x=5, palette 4, tile 0x40. */
    put(0, /*a0*/ uint16_t(5 | (0 << 14)),
           /*a1*/ uint16_t(5 | (2 << 14)),
           /*a2*/ uint16_t(0x40 | (4 << 12)));
    Lsw2ObjSprite s;
    check(lsw2_obj_decode(g_oam, 0, &s), "entry 0 decodes");
    check(s.x == 5 && s.y == 5, "at the slot column's origin");
    check(s.width == 32 && s.height == 32, "32x32");
    check(s.palette == 4, "palette 4 - player one's colour");
    check(s.tile == 0x40, "and its tile index");
    check(!s.disabled && !s.affine, "enabled and not affine");

    /* --- X IS SIGNED, AND NINE BITS -------------------------------------- */
    /* -8 is 0x1F8 in nine bits. Read as unsigned that is 504, which puts the
     * sprite off the right of a 256-pixel screen instead of off the left. */
    put(1, uint16_t(10), uint16_t((0x1F8u & 0x1FFu) | (2u << 14)), 0);
    lsw2_obj_decode(g_oam, 1, &s);
    check(s.x == -8, "a sprite hanging off the left edge reads negative");

    /* And the largest positive x still fits. */
    put(2, uint16_t(10), uint16_t(255u | (2u << 14)), 0);
    lsw2_obj_decode(g_oam, 2, &s);
    check(s.x == 255, "while 255 stays positive");

    /* --- BIT 9 MEANS TWO THINGS ------------------------------------------ */
    /* Affine CLEAR + bit 9 SET = disabled. */
    put(3, uint16_t(10 | 0x0200u), uint16_t(0 | (2u << 14)), 0);
    lsw2_obj_decode(g_oam, 3, &s);
    check(s.disabled, "with affine clear, bit 9 disables the sprite");

    /* Affine SET + bit 9 SET = double size, NOT disabled. Reading bit 9 as
     * disabled unconditionally hides every double-sized rotating sprite. */
    put(4, uint16_t(10 | 0x0100u | 0x0200u), uint16_t(0 | (2u << 14)), 0);
    lsw2_obj_decode(g_oam, 4, &s);
    check(s.affine, "with affine set, the entry is affine");
    check(!s.disabled, "and bit 9 is double-size, not disabled");

    /* --- 256-colour mode has no palette bank ----------------------------- */
    put(5, uint16_t(10 | 0x2000u), uint16_t(0 | (2u << 14)),
           uint16_t(0 | (7u << 12)));
    lsw2_obj_decode(g_oam, 5, &s);
    check(s.colour256, "the 256-colour bit is read");
    check(s.palette == 0,
          "and the palette field is zeroed, not passed through as a bank");

    /* --- priority --------------------------------------------------------- */
    put(6, uint16_t(10), uint16_t(0 | (2u << 14)), uint16_t(2u << 10));
    lsw2_obj_decode(g_oam, 6, &s);
    check(s.priority == 2, "priority comes from bits 10-11");

    /* --- bounds ----------------------------------------------------------- */
    check(!lsw2_obj_decode(nullptr, 0, &s), "a null table decodes nothing");
    check(!lsw2_obj_decode(g_oam, -1, &s), "nor a negative index");
    check(!lsw2_obj_decode(g_oam, kLsw2OamEntries, &s), "nor one past the end");
    check(lsw2_obj_decode(g_oam, kLsw2OamEntries - 1, &s),
          "but the last entry is in range");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("all checks passed\n");
    return 0;
}
