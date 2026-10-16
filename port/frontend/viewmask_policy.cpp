/* The pure half of viewmask.h - the game's own index arithmetic, reproduced
 * exactly and testable without a guest.
 *
 * Every branch below is a transcription of `0x020AF718`. Where the game does
 * something that looks redundant it is kept, because "it looked redundant" is
 * how the stride came to be applied to an index the game resolves first. */

#include "viewmask.h"

int lsw2_viewmask_resolve(unsigned mask, int index) {
    /* NEGATIVE IS NOT OURS TO ANSWER. The game asks the session authority
     * (`0x020AF754`, vtable slot 11) which view is active. A pure function has
     * nobody to ask, and inventing an answer here is precisely the class of
     * guess this file exists to end. */
    if (index < 0) return kLsw2ViewNotFound;

    /* RECORD 0 IS UNCONDITIONAL. `020AF728: CMP r1,#0 / BNE` falls through to
     * the arithmetic with r1 = 0, so index 0 never consults the mask - not even
     * to check that bit 0 is set. A caller asking for view 0 in an empty
     * session gets record 0, and so does the port. */
    if (index == 0) return 0;

    /* THE ORDINAL WALK. Target is index+1 (`020AF774: ADD r4, r1, #1`), and the
     * answer is the bit position at which the running popcount first reaches
     * it. Bits 0..3 only - the loop's bound is `CMP r8, #4`. */
    const int target = index + 1;
    int seen = 0;
    for (int bit = 0; bit < kLsw2ViewRecords; ++bit) {
        if (mask & (1u << bit)) {
            ++seen;
            if (seen == target) return bit;
        }
    }
    return kLsw2ViewNotFound;
}

int lsw2_viewmask_count(unsigned mask, int slots) {
    /* The game's bound comes from `[0x020FEB88 + 4]` and is re-read every
     * iteration, so a bound that shrinks mid-loop shortens it. Clamping here
     * rather than trusting the caller keeps a negative or oversized slot count
     * from walking past the four bits the mask actually has. */
    if (slots < 0) slots = 0;
    if (slots > kLsw2ViewRecords) slots = kLsw2ViewRecords;

    int n = 0;
    for (int bit = 0; bit < slots; ++bit) {
        if (mask & (1u << bit)) ++n;
    }
    return n;
}

uint32_t lsw2_viewmask_record(uint32_t manager, int resolved) {
    /* THE FALLBACK IS NOT ELEMENT FOUR. `0x240 + 4*0x70 == 0x400`, and the game
     * uses `0x440`. Deriving it from the stride would read a record 0x40 bytes
     * before the one the game means. */
    if (resolved < 0 || resolved >= kLsw2ViewRecords) {
        return manager + kLsw2ViewFallback;
    }
    return manager + kLsw2ViewArray + uint32_t(resolved) * kLsw2ViewStride;
}

unsigned lsw2_viewmask_compose(int local_station, const int* peer_stations,
                               int peer_count) {
    unsigned mask = 0;

    /* OURSELVES FIRST, and unconditionally. A client is not always station 0 -
     * `local_station()` exists precisely because assuming it was hid this
     * machine from its own roster once already. */
    if (local_station >= 0 && local_station < kLsw2ViewRecords) {
        mask |= 1u << local_station;
    }

    if (peer_stations == nullptr) return mask;

    for (int i = 0; i < peer_count; ++i) {
        const int s = peer_stations[i];

        /* -1 MEANS "HAS NOT SAID", not "station -1". A row exists from the
         * moment a peer connects; its station arrives with the first message
         * that carries one. Seating a bit for a row that has not reported would
         * make the game wait on input from a station nobody owns. */
        if (s < 0 || s >= kLsw2ViewRecords) continue;
        mask |= 1u << s;
    }
    return mask;
}
