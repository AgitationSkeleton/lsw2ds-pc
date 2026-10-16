/* See cartroster.h. Pure - no guest, no framework - so test_cartroster.cpp can
 * link the decision alone, the split hudportrait_policy.cpp also makes. */

#include "cartroster.h"

int lsw2_cartroster_remote_peers(const Lsw2CartSlot slots[kLsw2CartMaxSlots],
                                 int local_station, int seat_count,
                                 const char* out[3], int out_station[3]) {
    int n = 0;
    /* At most three peers besides the local player, and never more than the
     * cartridge says are seated. seat_count includes the local player, so the
     * peers are seat_count - 1 of them - but a name slot is the ground truth,
     * and the count only caps how many to trust in case a slot lingers. */
    const int cap = (seat_count > 1) ? (seat_count - 1) : 0;
    for (int i = 0; i < kLsw2CartMaxSlots && n < 3 && n < cap; ++i) {
        if (i == local_station) continue;      /* that seat is the local player */
        if (!slots[i].present) continue;        /* empty */
        if (slots[i].name[0] == '\0') continue; /* present but blank - not a peer */
        out[n] = slots[i].name;
        out_station[n] = i;
        ++n;
    }
    return n;
}
