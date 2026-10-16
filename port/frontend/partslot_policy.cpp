/* The pure half of partslot.h - see there for the derivation. */

#include "partslot.h"

namespace {

/* The five ranges and their tables, in the categoriser's own order. The upper
 * bound is INCLUSIVE, as `CMP r0,#0x2C / MOVLS` is. */
struct Category {
    int lo;
    int hi;
    uint32_t table;
    uint32_t stride;
};

constexpr Category kCategories[5] = {
    {0x00, 0x2C, 0x020F2414u, 0x28u},
    {0x2D, 0x6A, 0x020F3E68u, 0x2Cu},
    {0x6B, 0xB0, 0x020F3490u, 0x24u},
    {0xB1, 0xE7, 0x020F2B1Cu, 0x2Cu},
    {0xE8, 0xF9, 0x020F2144u, 0x28u},
};

}  // namespace

int lsw2_part_category(int part_id) {
    /* NEGATIVE IS NOT CATEGORY 0. The game's compares are unsigned (`MOVLS`),
     * so a negative id would wrap to something enormous and land in category 5.
     * Saying so explicitly beats relying on a sign the C++ type does not have.
     */
    if (part_id < 0) return 5;

    for (int i = 0; i < 5; ++i) {
        if (part_id >= kCategories[i].lo && part_id <= kCategories[i].hi) {
            return i;
        }
    }
    return 5;
}

uint32_t lsw2_part_descriptor(int part_id) {
    const int cat = lsw2_part_category(part_id);
    if (cat >= 5) return 0;   /* 0x0208DB64: a null descriptor, not a pointer */

    const Category& c = kCategories[cat];
    return c.table + uint32_t(part_id - c.lo) * c.stride;
}

uint32_t lsw2_actor_slot_addr(uint32_t actor, int slot) {
    return actor + kLsw2ActorSlotTable + uint32_t(slot) * 2u;
}
