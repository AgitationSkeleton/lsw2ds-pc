/* See netcount.h. */

#include "netcount.h"

#include "ui.h"

#include "state.h"

#include <cstdio>

namespace {

/* The per-player input object. Four records of 0x0A bytes, then three
 * header bytes - `+0x28` unread, `+0x29` the count, `+0x2A` the local station. */
/* See netcount.h - the base and the count offset live there. */

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

int g_written = -2;   /* -2 so the first observation always prints */

uint8_t read_byte(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

}  // namespace

int lsw2_netcount_guest() {
    if (kLsw2NetCountObject < kRamLow || kLsw2NetCountObject >= kRamHigh) return -1;
    return int(read_byte(kLsw2NetCountObject + kLsw2NetPlayerCount));
}

void lsw2_netcount_tick() {
    if (!ui_fork_build()) return;

    /* THE WRITE IS GONE, and its absence is the fix.
     *
     * This function used to mirror the session's seated count into
     * `[0x021027F8 + 0x29]`. Two things have since been read out of the ROM,
     * and either one alone retires the write:
     *
     *   1. The receive pump does not read that byte. It reads `[0x020FEB88]`
     *      so raising this one never enabled anything.
     *
     *   2. Both bytes are DERIVED. `0x02072110` popcounts the view mask into
     *      `[0x02104D2C+6]`, then hands the same number to `0x0205CE5C` (this
     *      byte) and `0x02073054` (the pump's word) four instructions apart.
     *      A port write to any of them survives until the game next runs that
     *      derivation and no longer.
     *
     * So the write was a value the game recomputes, into a byte the pump does
     * not read - a workaround that masks the cause rather than fixing it. Removing it
     * also removes a real hazard: this byte sits in the same word as `+0x2A`,
     * the local station index, and it bounds the four input records it lives
     * among. Writing a count the game disagrees with is a way to make input
     * read from the wrong player.
     *
     * The lever is the mask - see viewmask.h. This function is now only the
     * diagnostic that shows what the game decided, which is what should have
     * been built first. */
    const int now = lsw2_netcount_guest();
    if (now < 0) return;
    if (now == g_written) return;
    g_written = now;

    std::fprintf(stderr,
                 "[fork] the game set its input player count to %d "
                 "(derived from the view mask; the port does not write it)\n",
                 now);
}
