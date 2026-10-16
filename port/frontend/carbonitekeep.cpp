/* See carbonitekeep.h. */

#include "carbonitekeep.h"

#include "cheats.h"      /* kLsw2ActorCharacter */
#include "customization.h"  /* kLsw2CustomNone - the game's empty slot */
#include "gearapply.h"   /* kLsw2GearApplier */
#include "headfix.h"     /* kLsw2StockParts, kLsw2StockStride */
#include "partslot.h"    /* lsw2_actor_slot_addr */
#include "ui.h"

#include "state.h"
#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>

extern "C" void lsw2_arm9_FUN_02006220(void);
extern "C" void lsw2_arm9_FUN_020063d4(void);

namespace {

/* The slot the Carbonite is carried in, and the two carry flags the reset
 * clears with it (0x02006270..0x02006278). */
constexpr int kCarriedSlot = 4;
constexpr uint32_t kCarryFlagA = 0x1DFu;
constexpr uint32_t kCarryFlagB = 0x207u;

int g_kept = 0;
constexpr int kKeptReports = 8;

uint8_t rd8(uint32_t addr) {
    const uint32_t w = bus_device_read32(9, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

void wr8(uint32_t addr, uint8_t v) {
    const uint32_t sh = (addr & 3u) * 8u;
    const uint32_t w = bus_device_read32(9, addr & ~3u);
    bus_device_write32(9, addr & ~3u, (w & ~(0xFFu << sh)) | (uint32_t(v) << sh));
}

void wr16(uint32_t addr, uint16_t v) {
    const uint32_t sh = (addr & 2u) * 8u;
    const uint32_t w = bus_device_read32(9, addr & ~3u);
    bus_device_write32(9, addr & ~3u, (w & ~(0xFFFFu << sh)) | (uint32_t(v) << sh));
}

}  // namespace

void lsw2_carbonitekeep_override() {
    static const bool off = std::getenv("LSW2_CARBONITE_KEEP_OFF") != nullptr;
    if (g_cpu.R[14] == kLsw2CaptureResetReturn && ui_fork_build() && !off) {
        const uint32_t actor = g_cpu.R[0];
        const uint32_t transform = g_cpu.R[1];
        const uint8_t character = rd8(actor + kLsw2ActorCharacter);
        const uint8_t stock =
            rd8(kLsw2StockParts + uint32_t(character) * kLsw2StockStride + kCarriedSlot);

        ++g_kept;
        if (g_kept <= kKeptReports) {
            std::fprintf(stderr,
                         "[fork] capture: actor %08X (character %u) gives up "
                         "the Carbonite and keeps its customization - slot %d "
                         "back to %s %u, the rest untouched (%d so far)\n",
                         unsigned(actor), unsigned(character), kCarriedSlot,
                         stock == kLsw2CustomNone ? "empty" : "stock part",
                         unsigned(stock), g_kept);
        }

        if (stock != kLsw2CustomNone) {
            /* THE RESET'S OWN STEP FOR THIS SLOT (0x02006254..0x0200625C): the
             * gear applier with the stock part and the reset's own transform.
             * It returns to the handler, whose address is still in R[14]. */
            g_cpu.R[1] = stock;
            g_cpu.R[2] = transform;
            g_cpu.R[15] = kLsw2GearApplier;
            lsw2_arm9_FUN_020063d4();
            return;
        }

        /* ...OR ITS OTHER BRANCH (0x02006264..0x02006278): the slot emptied
         * and, this being slot 4, both carry flags cleared. Then back to the
         * handler: the other four slots and the weapon are what the player
         * chose, and the capture has no business with them. */
        wr16(lsw2_actor_slot_addr(actor, kCarriedSlot), uint16_t(kLsw2CustomNone));
        wr8(actor + kCarryFlagA, 0u);
        wr8(actor + kCarryFlagB, 0u);
        g_cpu.R[15] = g_cpu.R[14];
        return;
    }

    /* EVERY OTHER RESET: the game's own function, unchanged. */
    g_cpu.R[15] = kLsw2StockAppearance;
    lsw2_arm9_FUN_02006220();
}
