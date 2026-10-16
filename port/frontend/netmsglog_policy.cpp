/* The pure half of netmsglog.h - the slot walk, testable without a guest. */

#include "netmsglog.h"

int lsw2_netmsg_walk(uint32_t station_base,
                     uint32_t (*read32)(uint32_t addr, void* ctx), void* ctx,
                     void (*out)(int slot, int type, int seq, void* ctx)) {
    if (!read32) return 0;

    /* SIXTEEN VISITS, and the number is a guess with a reason. The list is
     * guest memory: a corrupt delta can point backwards and make a cycle, and
     * a cycle here would hang the frame rather than print a wrong number. The
     * game's own loop has no such guard because its data is its own. */
    constexpr int kMaxVisits = 16;

    int slot = 0;
    int visited = 0;
    while (visited < kMaxVisits) {
        const uint32_t entry = read32(station_base + uint32_t(slot) * 4u, ctx);

        /* THE LOW BYTE IS THE DELTA, and zero ends the list - `TST r0, #0xFF`
         * at 0x020B04C8 is exactly this test. */
        const int delta = int(entry & 0xFFu);
        if (delta == 0) break;

        ++visited;
        if (out) {
            out(slot, int((entry >> 8) & 0xFFu),
                int((entry >> 16) & 0xFFFFu), ctx);
        }

        /* `ADD r2, r4, r2` then `AND r4, r2, #0xFF` - the cursor wraps in a
         * byte, so a large delta walks round rather than off the end. */
        slot = (slot + delta) & 0xFF;
    }
    return visited;
}
