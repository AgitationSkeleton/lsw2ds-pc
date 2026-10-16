/* The guest half of partslot.h: the slot byte lives inside the descriptor. */

#include "partslot.h"

#include "state.h"

int lsw2_part_slot(int part_id) {
    const uint32_t desc = lsw2_part_descriptor(part_id);
    if (desc == 0) return -1;

    /* A BYTE at `+8`, read through its word. `0x02006450` is `LDRB r0,
     * [r4, #8]` - one byte, and the width matters: the neighbouring bytes in
     * that word are other descriptor fields, and reading the halfword or word
     * here would return a number that looks like a slot and is not. */
    const uint32_t addr = desc + 8u;
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return int((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

/* --- the probe ------------------------------------------------------------ */

#include "customization.h"
#include "cheats.h"
#include "ui.h"

#include <cstdio>

namespace {

/* The customize panel's choices - FIVE PARTS AND A WEAPON, not six parts.
 *
 * The store holds six halfwords and the figure has only five slots. The first
 * five go to the gear applier, one per slot; the sixth at `+0xB0` goes to
 * `0x02004404`, which writes the action row at `actor + 0x6C3`. So the panel
 * stores exactly what customization needs - attachments and weapon changes -
 * and the port replays five calls plus one.
 *
 * The game reads any of it only for character 72, which is why the
 * fork has to replay it. */
/* See customization.h for the base and the slot offsets. */
constexpr int kCustomPartCount = 5;      /* indices 0..4 are parts */
constexpr int kCustomWeaponIndex = 5;    /* index 5 is the action row */
/* The action row is cheats.h's kLsw2ActorActionRow. */

uint16_t read16(uint32_t addr) {
    const uint32_t w = bus_device_read32(0, addr & ~3u);
    return uint16_t((w >> ((addr & 2u) * 8u)) & 0xFFFFu);
}

uint32_t g_last_actor = 0;
uint16_t g_last_parts[6] = {0, 0, 0, 0, 0, 0};

}  // namespace

void lsw2_partslot_probe() {
    if (!ui_fork_build()) return;

    const uint32_t actor = lsw2_cheats_party_actor(0);
    if (actor < 0x02000000u || actor >= 0x02400000u) return;

    uint16_t parts[6];
    bool changed = (actor != g_last_actor);
    for (int i = 0; i < 6; ++i) {
        parts[i] = read16(kLsw2CustomBase + kLsw2CustomSlotOffsets[i]);
        if (parts[i] != g_last_parts[i]) changed = true;
    }
    if (!changed) return;

    g_last_actor = actor;
    for (int i = 0; i < 6; ++i) g_last_parts[i] = parts[i];

    std::fprintf(stderr, "[fork] customize parts for actor %08X (character %d):\n",
                 actor, lsw2_cheats_party_character(0));

    /* WHAT THE PORT WANTS, BESIDE WHAT THE GAME HAS.
     *
     * Every row below reads the GUEST's customize slots. That answers "what is
     * the game holding" and was mistaken for "what did the player choose" for
     * a long stretch of debugging: the guest slots read 0 while
     * the port's own saved set held something else, and the two were never
     * printed together so the disagreement was invisible.
     *
     * `saved` is the raw persisted set and `want` is that set after the host's
     * policy filter - the one the gear applier actually acts on. Printing both
     * is what tells "the player chose nothing" from "the host disallowed it"
     * from "the port never loaded it". */
    {
        const uint16_t* saved = lsw2_customization_saved();
        uint16_t want[kLsw2CustomSlots];
        lsw2_customization_effective(want);
        std::fprintf(stderr, "   port saved:");
        for (int i = 0; i < kLsw2CustomSlots; ++i)
            std::fprintf(stderr, " %3u", unsigned(saved[i]));
        std::fprintf(stderr, "\n   after the host's policy:");
        for (int i = 0; i < kLsw2CustomSlots; ++i)
            std::fprintf(stderr, " %3u", unsigned(want[i]));
        std::fprintf(stderr, "   (%u = the game's empty)\n",
                     unsigned(kLsw2CustomNone));
    }
    for (int i = 0; i < kCustomPartCount; ++i) {
        const int slot = lsw2_part_slot(int(parts[i]));
        if (slot < 0) {
            std::fprintf(stderr,
                         "   [%d] part %3d -> no descriptor (>= 0xFA)\n",
                         i, int(parts[i]));
            continue;
        }
        /* WHAT THE ACTOR IS WEARING THERE. Slot 0 is the head, so
         * this column is also what decides the helmet enhancement: if a helmet-class character
         * reads empty here while wearing a hat, the enhancement fills slot 0
         * rather than replacing something. */
        const uint16_t worn = read16(lsw2_actor_slot_addr(actor, slot));
        std::fprintf(stderr,
                     "   [%d] part %3d -> slot %d; the actor wears %d there\n",
                     i, int(parts[i]), slot, int(worn));
    }

    /* THE WEAPON, which is not a part and does not go through a slot. */
    const uint32_t row_addr = actor + kLsw2ActorActionRow;
    const uint32_t w = bus_device_read32(0, row_addr & ~3u);
    const int worn_row = int((w >> ((row_addr & 3u) * 8u)) & 0xFFu);
    std::fprintf(stderr,
                 "   [w] action row %3d chosen; the actor carries %d\n",
                 int(parts[kCustomWeaponIndex]), worn_row);
}
