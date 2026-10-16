/* The part-id arithmetic, pinned against two independent derivations.
 *
 * The eight headwear cases below are the point of this file. Their descriptor
 * addresses come from the table arithmetic in partslot_policy.cpp; the fact
 * that every one lands on a byte reading 3 was checked against
 * config/customization_modes.json, which recorded slot_index 3 for all eight by
 * an unrelated method. Neither source could have copied the other.
 *
 * The boundary cases are the second point. An off-by-one on any range end does
 * not fail loudly - it lands in the NEXT table and returns a plausible
 * descriptor for the wrong part, which is the failure mode this project has hit
 * repeatedly and which no runtime check would catch. */

#include "partslot.h"

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
    /* --- the categoriser's five ranges ------------------------------------ */
    check(lsw2_part_category(0x00) == 0, "part 0x00 is category 0");
    check(lsw2_part_category(0x2C) == 0, "0x2C is the last of category 0");
    check(lsw2_part_category(0x2D) == 1, "0x2D is the first of category 1");
    check(lsw2_part_category(0x6A) == 1, "0x6A is the last of category 1");
    check(lsw2_part_category(0x6B) == 2, "0x6B is the first of category 2");
    check(lsw2_part_category(0xB0) == 2, "0xB0 is the last of category 2");
    check(lsw2_part_category(0xB1) == 3, "0xB1 is the first of category 3");
    check(lsw2_part_category(0xE7) == 3, "0xE7 is the last of category 3");
    check(lsw2_part_category(0xE8) == 4, "0xE8 is the first of category 4");
    check(lsw2_part_category(0xF9) == 4, "0xF9 is the last of category 4");
    check(lsw2_part_category(0xFA) == 5, "0xFA has no descriptor");
    check(lsw2_part_category(0xFFFF) == 5, "a large id has no descriptor");
    check(lsw2_part_category(-1) == 5,
          "a negative id is refused, not wrapped into category 0");

    /* --- the descriptor addresses, at each table's first entry ------------- */
    check(lsw2_part_descriptor(0x00) == 0x020F2414u, "category 0 starts at 0x020F2414");
    check(lsw2_part_descriptor(0x2D) == 0x020F3E68u, "category 1 starts at 0x020F3E68");
    check(lsw2_part_descriptor(0x6B) == 0x020F3490u, "category 2 starts at 0x020F3490");
    check(lsw2_part_descriptor(0xB1) == 0x020F2B1Cu, "category 3 starts at 0x020F2B1C");
    check(lsw2_part_descriptor(0xE8) == 0x020F2144u, "category 4 starts at 0x020F2144");
    check(lsw2_part_descriptor(0xFA) == 0u,
          "an id past the tables gives a null descriptor, not a pointer");

    /* Each table's stride, one entry in. */
    check(lsw2_part_descriptor(0x01) == 0x020F2414u + 0x28u, "category 0 strides 0x28");
    check(lsw2_part_descriptor(0x2E) == 0x020F3E68u + 0x2Cu, "category 1 strides 0x2C");
    check(lsw2_part_descriptor(0x6C) == 0x020F3490u + 0x24u, "category 2 strides 0x24");
    check(lsw2_part_descriptor(0xB2) == 0x020F2B1Cu + 0x2Cu, "category 3 strides 0x2C");
    check(lsw2_part_descriptor(0xE9) == 0x020F2144u + 0x28u, "category 4 strides 0x28");

    /* --- THE CROSS-VALIDATED EIGHT ----------------------------------------
     * config/customization_modes.json's headwear, whose descriptors were
     * confirmed to hold slot 3 at these exact addresses. */
    check(lsw2_part_descriptor(178) == 0x020F2B48u, "BaseballCap (178) -> 0x020F2B48");
    check(lsw2_part_descriptor(187) == 0x020F2CD4u, "CowboyHat (187) -> 0x020F2CD4");
    check(lsw2_part_descriptor(188) == 0x020F2D00u, "Crown (188) -> 0x020F2D00");
    check(lsw2_part_descriptor(202) == 0x020F2F68u, "KnightHelmet (202) -> 0x020F2F68");
    check(lsw2_part_descriptor(212) == 0x020F3120u, "Moustache (212) -> 0x020F3120");
    check(lsw2_part_descriptor(214) == 0x020F3178u, "PirateHat (214) -> 0x020F3178");
    check(lsw2_part_descriptor(228) == 0x020F33E0u, "TopHat (228) -> 0x020F33E0");
    check(lsw2_part_descriptor(229) == 0x020F340Cu, "WizardHat (229) -> 0x020F340C");

    /* All eight are category 3, which is the reason they share a slot. */
    check(lsw2_part_category(178) == 3 && lsw2_part_category(229) == 3,
          "the headwear range sits wholly inside category 3");

    /* --- the actor's own slot record --------------------------------------- */
    const uint32_t actor = 0x02200000u;
    check(lsw2_actor_slot_addr(actor, 0) == actor + 0x6E8u, "slot 0 at actor+0x6E8");
    check(lsw2_actor_slot_addr(actor, 3) == actor + 0x6E8u + 6u,
          "slot 3 is three halfwords on");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
