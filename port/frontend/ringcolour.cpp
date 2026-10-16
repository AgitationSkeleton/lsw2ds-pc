/* See ringcolour.h. */

#include "ringcolour.h"

#include "forksession.h"
#include "hudportrait.h"   /* kLsw2HudRingSlot, kLsw2LevelStateHud */
#include "statelog.h"
#include "ui.h"

#include "io.h"            /* nds_cartridge_bytes - patch 0019 */
#include "state.h"
#include "vram.h"          /* nds_vram_*_texpal16 - patch 0020 */

#include <cstdio>

namespace {

constexpr uint32_t kSpritePalette = 0xD8u;   /* sprite -> its palette object */
constexpr uint32_t kPaletteColours = 0x04u;  /* -> the RAM copy of the colours */
constexpr uint32_t kPaletteVram = 0x08u;     /* low half: texpal offset / 8 */
constexpr uint32_t kPaletteBytes = 0x10u;    /* the colours' size in bytes */
constexpr int kColours = 16;

/* Which of the ROM's five palettes each slot wears - the four a match stages
 * into the touch screen's OBJ banks 4..7 (ringcolour.h). */
constexpr int kRomPaletteOfSlot[4] = {0, 1, 3, 4};

bool ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }
uint32_t r32(uint32_t a) { return bus_device_read32(0, a); }
uint16_t r16(uint32_t a) {
    return uint16_t(bus_device_read32(0, a & ~3u) >> ((a & 2u) * 8u));
}

/* The ROM's palettes, read once. False when the cartridge is not there or too
 * short to hold them. */
bool rom_palettes(uint16_t out[5][kColours]) {
    static bool done = false, ok = false;
    static uint16_t cache[5][kColours];
    if (!done) {
        done = true;
        const uint8_t* rom = nds_cartridge_bytes();
        if (rom && nds_cartridge_size() >= kLsw2PlayerPalettesRom + 5u * 0x20u) {
            for (int p = 0; p < 5; ++p)
                for (int c = 0; c < kColours; ++c) {
                    const uint8_t* b = rom + kLsw2PlayerPalettesRom + p * 0x20 + c * 2;
                    cache[p][c] = uint16_t(b[0] | (b[1] << 8));
                }
            ok = true;
        }
    }
    if (ok)
        for (int p = 0; p < 5; ++p)
            for (int c = 0; c < kColours; ++c) out[p][c] = cache[p][c];
    return ok;
}

uint32_t g_said_vram = 0xFFFFFFFFu;
int g_said_slot = -2;
bool g_said_decline = false;

/* What this module last wrote, so it can recognise its own recolour (and put
 * the game's colours back) without mistaking anything else for it. */
uint32_t g_written_vram = 0xFFFFFFFFu;
uint16_t g_written[16] = {0};

}  // namespace

void lsw2_ringcolour_tick() {
    /* THE RING'S PALETTE, through the game's own objects. */
    const uint32_t world = r32(kLsw2StateManager + kLsw2SlotResult +
                               kLsw2WorldSlot * 4u);
    if (!ram(world) || r32(world + kLsw2StateKind) != kLsw2LevelStateKind) return;
    const uint32_t ring = r32(world + kLsw2LevelStateHud + kLsw2HudRingSlot);
    if (!ram(ring)) return;
    const uint32_t pal = r32(ring + kSpritePalette);
    if (!ram(pal)) return;
    const uint32_t colours = r32(pal + kPaletteColours);
    if (!ram(colours) || r32(pal + kPaletteBytes) != kColours * 2u) return;
    const uint32_t vram = (r32(pal + kPaletteVram) & 0xFFFFu) * 8u;

    uint16_t own[kColours];
    for (int i = 0; i < kColours; ++i) own[i] = r16(colours + uint32_t(i) * 2u);

    /* THE LOCAL SLOT. Player 1, no session, or the setting off: the game's own
     * colours, which also puts them back after a recolour. */
    const Lsw2ForkSession& s = lsw2_fork_session();
    const bool on = lsw2_ui_enhanced_slot_colour() &&
                    s.state() != Lsw2SessionState::Offline;
    const int slot = on ? s.local_station() : 0;

    uint16_t want[kColours];
    for (int i = 0; i < kColours; ++i) want[i] = own[i];
    if (slot > 0 && slot < 4) {
        uint16_t pals[5][kColours];
        if (!rom_palettes(pals)) return;
        const uint16_t* blue = pals[kRomPaletteOfSlot[0]];
        const uint16_t* mine = pals[kRomPaletteOfSlot[slot]];
        for (int i = 0; i < kColours; ++i) {
            int j = 0;
            while (j < kColours && (blue[j] & 0x7FFFu) != (own[i] & 0x7FFFu)) ++j;
            if (j == kColours) {
                /* NOT THE RING THIS WAS MEASURED ON: decline, and say so once. */
                if (!g_said_decline) {
                    g_said_decline = true;
                    std::fprintf(stderr,
                                 "[fork] ring colour: colour %d (%04X) of the "
                                 "ring's palette is not in the game's blue "
                                 "player palette - leaving the ring alone\n",
                                 i, unsigned(own[i]));
                }
                return;
            }
            want[i] = uint16_t(mine[j] & 0x7FFFu);
        }
    }

    /* ONLY OVER THE RING'S OWN PALETTE. What the texture palette holds right
     * now must be the game's upload of these colours, or this module's own
     * earlier recolour of them - anything else means the ring's palette is not
     * there this frame (not uploaded yet, or a level's palettes moving under a
     * stale object), and writing would repaint somebody else's. Measured
     * (conn_ring2): the host found neither at 0x3760 on one frame. */
    uint16_t now[kColours];
    bool is_want = true, is_own = true, is_ours = vram == g_written_vram;
    for (int i = 0; i < kColours; ++i) {
        now[i] = uint16_t(nds_vram_read_texpal16(vram + uint32_t(i) * 2u) & 0x7FFFu);
        is_want = is_want && now[i] == (want[i] & 0x7FFFu);
        is_own = is_own && now[i] == (own[i] & 0x7FFFu);
        is_ours = is_ours && now[i] == (g_written[i] & 0x7FFFu);
    }
    if (is_want || (!is_own && !is_ours)) return;

    /* ONLY WHAT DIFFERS: a write bumps the texture generation. */
    int written = 0;
    for (int i = 0; i < kColours; ++i) {
        if (now[i] == (want[i] & 0x7FFFu)) continue;
        if (nds_vram_write_texpal16(vram + uint32_t(i) * 2u, want[i])) ++written;
    }
    g_written_vram = slot > 0 ? vram : 0xFFFFFFFFu;
    for (int i = 0; i < kColours; ++i) g_written[i] = want[i];
    if (written && (vram != g_said_vram || slot != g_said_slot)) {
        g_said_vram = vram;
        g_said_slot = slot;
        std::fprintf(stderr,
                     "[fork] ring colour: the top-screen ring's palette "
                     "(texpal %05X) now wears slot %d's colours (%d written)\n",
                     unsigned(vram), slot, written);
    }
}
