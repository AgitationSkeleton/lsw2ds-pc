/* The pure half of coophosts.h - see there for the derivation. */

#include "coophosts.h"

int lsw2_coophosts_rows(int wanted) {
    if (wanted <= 0) return 0;
    return wanted < kLsw2HostSlots ? wanted : kLsw2HostSlots;
}

uint16_t lsw2_coophosts_status() {
    /* The list skips a record whose low two bits are 2:
     *
     *     02070400  ldrh r0, [r9, #0x48]
     *     02070404  and  r0, r0, #3
     *     02070408  cmp  r0, #2
     *     0207040C  addeq r5, r5, #1      skipped
     *
     * So 0, 1 and 3 all list. ONE is chosen rather than zero because zero is
     * also what an untouched record holds, and a value that means "published"
     * should not be the value that means "never written" - the two are
     * distinguishable in a memory dump only if they differ. */
    return 1u;
}
