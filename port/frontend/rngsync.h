/* Keeping four machines' random numbers in step. See rngsync.cpp for why this
 * is necessary at all - the short version is that the game seeds MT19937 from the real-time clock, so no two machines ever
 * agree by accident.
 */

#ifndef LSW2_RNGSYNC_H
#define LSW2_RNGSYNC_H

#include <cstdint>

/* The game's RNG record. ONE COPY: diagnostics.cpp declared it too, under the
 * same name, so a correction to either would have missed the other. */
constexpr uint32_t kLsw2RandState = 0x02104344u;

/* Write the seed and force the game to expand it on its next draw. */
void lsw2_rng_apply_seed(uint32_t seed);

/* Once a frame in the fork. Applies the session's seed on the frame the player
 * enters an arena match, and only then. */
void lsw2_rng_sync_tick();

#endif /* LSW2_RNGSYNC_H */
