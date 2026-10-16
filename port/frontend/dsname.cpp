/* See dsname.h - the guest half. */

#include "dsname.h"

#include "forksession.h"

#include "state.h"

#include <cstdio>

namespace {

uint16_t rd16(uint32_t addr) {
    /* The block is halfword-aligned at +6, so read the containing word and
     * pick the half - the same shape slotcolour.cpp uses for OAM. */
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint16_t((addr & 2u) ? (w >> 16) : (w & 0xFFFFu));
}

/* Does the guest's block already hold exactly this name? Eleven halfword
 * reads a frame, which is what "idempotent" costs and is nothing. */
bool block_holds(const uint16_t want[kLsw2DsNicknameChars], int len) {
    const uint32_t base = kLsw2DsUserSettings + kLsw2DsNicknameOffset;
    for (int i = 0; i < kLsw2DsNicknameChars; ++i)
        if (rd16(base + uint32_t(i) * 2u) != want[i]) return false;
    return rd16(kLsw2DsUserSettings + kLsw2DsNicknameLenOffset) == uint16_t(len);
}

int g_writes = 0;

}  // namespace

void lsw2_dsname_tick() {
    uint16_t want[kLsw2DsNicknameChars];
    const int len = lsw2_dsname_encode(lsw2_fork_session().player_name(), want);

    /* READ BACK BEFORE BELIEVING. An injected write that reverts within a frame
     * is a known shape on this project; comparing the block every frame means
     * a guest that restores the firmware's name is caught and counted rather
     * than assumed away. */
    if (block_holds(want, len)) return;

    const uint32_t base = kLsw2DsUserSettings + kLsw2DsNicknameOffset;
    for (int i = 0; i < kLsw2DsNicknameChars; ++i)
        bus_device_write16(0, base + uint32_t(i) * 2u, want[i]);
    bus_device_write16(0, kLsw2DsUserSettings + kLsw2DsNicknameLenOffset,
                       uint16_t(len));

    ++g_writes;
    char shown[kLsw2DsNicknameChars + 1];
    for (int i = 0; i < len; ++i) shown[i] = char(want[i]);
    shown[len] = '\0';
    /* Once is the expected count. A second line means the guest put its own
     * name back and the write raced it - worth reading, not a quiet fix. */
    std::fprintf(stderr,
                 "[fork] DS nickname set to \"%s\" (%d chars) in the user "
                 "settings block at %08X - write %d; the cartridge's beacon, "
                 "browse row and peer slots now carry the player's name\n",
                 shown, len, unsigned(kLsw2DsUserSettings), g_writes);
}
