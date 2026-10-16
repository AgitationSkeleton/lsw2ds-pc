/* See slotcolour.h. The guest half. */

#include "slotcolour.h"

#include "forksession.h"
#include "objsprite.h"
#include "screenengine.h"
#include "state.h"
#include "ui.h"
#include "vram.h"

#include <cstdio>

namespace {

/* === SUPERSEDED 2026-08-31, AND IT WAS LOAD-BEARING ======================
 * | The TOP screen is the main engine. That is the whole reason the        |
 * | predicate can be as simple as "palette 4..7": the layout capture says  |
 * | the in-match top screen is "portrait plus hearts only", so nothing     |
 * | else there wears a player colour. The touch screen's slot column also  |
 * | uses 4..7 and is deliberately NOT touched - those rows are the OTHER   |
 * | players and already wear their own.                                    |
 * ==========================================================================
 *
 * THE TOP SCREEN IS WHICHEVER ENGINE POWCNT1 SAYS. The paragraph above was
 * the justification for the whole predicate and it assumed a fixed assignment
 * the hardware does not promise and the captures contradict - see
 * screenengine.h for the two frames that disagree. Reading engine 0 when the
 * game has swapped them hands back the TOUCH screen's slot column, which is
 * exactly the set of sprites the paragraph above says are deliberately not
 * touched.
 *
 * So the engine is asked for per frame now, and the distinct-colour count
 * below is the second line of defence for the frames where the answer is
 * still not enough. */

int g_reports = 0;
constexpr int kReports = 4;
uint32_t g_said = 0;          /* bitmask of entries already reported */
bool g_said_empty = false;    /* the "nothing to recolour" line, said once */
bool g_said_column = false;   /* the "that is a slot column" line, said once */

}  // namespace

void lsw2_slotcolour_tick() {
    if (!lsw2_ui_enhanced_slot_colour()) return;

    /* THE LOCAL SLOT, and the session is the only thing that knows it. Offline
     * there is no slot and nothing to do - the ring is the host's blue and that
     * is correct. */
    const Lsw2ForkSession& s = lsw2_fork_session();
    if (s.state() == Lsw2SessionState::Offline) return;
    const int slot = s.local_station();
    if (lsw2_slot_palette(slot) < 0) return;

    const uint8_t* oam = nds_vram_renderer_oam(lsw2_screen_engine(kLsw2ScreenTop));
    if (!oam) return;

    /* COUNT THE DISTINCT PLAYER COLOURS BEFORE WRITING ANY OF THEM.
     *
     * THE PREDICATE BELOW IS "palette 4..7 on the main engine is the local
     * player's ring", and it rests on the layout capture's "portrait plus
     * hearts only". If more than one of the four player colours is on this
     * engine at once they cannot all be one player's ring - what is there is a
     * slot COLUMN, four other players' portraits - and repainting it would
     * recolour three other players to the local player's colour and destroy
     * the palette identification the match HUD's read_rows() derives players from. That
     * is the failure this file's header warns about for the sub engine, and it
     * arrives just as easily through the main one.
     *
     * MEASURED, NOT FEARED. 10,931 captured frames were sampled across 85
     * capture streams (every 25th of 286,097). 5,332 of them carry at least one
     * main-engine sprite in palette 4..7 - so the header's "oam_main is empty"
     * is false - and ALL 5,332, one hundred percent, carry all FOUR colours at
     * once. Not one observed frame matches the premise. In
     * gpudump_lobbywait/inst0 f025386 they are four 40x40 portrait cells at
     * x=9, y=29/69/109/149, each assembled from a 32x32, an 8x32 and a 32x8,
     * which is a slot column and not a ring.
     *
     * So this declines rather than guesses. The write still stands, unchanged,
     * for the case the original reasoning actually described - one player
     * colour on the screen - and that case has simply never been seen. */
    int distinct = 0;
    for (int i = 0; i < kLsw2OamEntries; ++i) {
        Lsw2ObjSprite probe;
        if (!lsw2_obj_decode(oam, i, &probe) || probe.disabled) continue;
        if (probe.palette < kLsw2FirstSlotPalette ||
            probe.palette >= kLsw2FirstSlotPalette + kLsw2SlotPalettes)
            continue;
        bool seen = false;
        for (int j = 0; j < i; ++j) {
            Lsw2ObjSprite earlier;
            if (!lsw2_obj_decode(oam, j, &earlier) || earlier.disabled) continue;
            if (earlier.palette == probe.palette) { seen = true; break; }
        }
        if (!seen) ++distinct;
    }

    if (distinct > 1) {
        if (!g_said_column) {
            g_said_column = true;
            std::fprintf(stderr,
                         "[fork] slot colour: %d different player colours are "
                         "on the main engine at once, so these are other "
                         "players' portraits and not this player's ring - "
                         "declining to repaint them. See slotcolour.cpp.\n",
                         distinct);
        }
        return;
    }

    int candidates = 0;
    for (int i = 0; i < kLsw2OamEntries; ++i) {
        Lsw2ObjSprite sp;
        if (!lsw2_obj_decode(oam, i, &sp) || sp.disabled) continue;
        if (sp.palette >= kLsw2FirstSlotPalette &&
            sp.palette < kLsw2FirstSlotPalette + kLsw2SlotPalettes)
            ++candidates;

        const int want = lsw2_slotcolour_rewrite(sp.palette, slot);
        if (want < 0) continue;

        /* WRITE THE FIELD THE GAME WRITES. Attribute 2's top four bits are the
         * palette bank; everything else in that halfword - the tile number and
         * the priority - is left exactly as it was. */
        const uint32_t addr = kLsw2OamBase + uint32_t(i) * kLsw2OamEntryStride +
                              kLsw2OamAttr2;
        const uint32_t word = bus_device_read32(0, addr & ~3u);
        const uint32_t shift = (addr & 2u) ? 16u : 0u;
        uint16_t attr2 = uint16_t((word >> shift) & 0xFFFFu);
        attr2 = uint16_t((attr2 & 0x0FFFu) | (uint16_t(want) << 12));
        bus_device_write32(0, addr & ~3u,
                           (word & ~(0xFFFFu << shift)) |
                               (uint32_t(attr2) << shift));

        /* SAY WHAT WAS RECOLOURED, once per entry. The predicate is an
         * inference from "the top screen is portrait plus hearts only"; if it
         * is too wide, this line names the sprite that proves it rather than
         * leaving a mysteriously tinted HUD to be puzzled over. */
        if (i < 32 && !(g_said & (1u << i)) && g_reports < kReports) {
            g_said |= 1u << i;
            ++g_reports;
            std::fprintf(stderr,
                         "[fork] slot colour: OAM %d at (%d,%d) %dx%d "
                         "palette %d -> %d (local slot %d)\n",
                         i, sp.x, sp.y, sp.width, sp.height, sp.palette, want,
                         slot);
        }
    }

    /* AND SAY SO WHEN THERE WAS NOTHING TO RECOLOUR, once, because silence
     * here is the one answer this code cannot distinguish from success.
     *
     * === SUPERSEDED 2026-08-31 BY COUNTING THE CAPTURES ===================
     * | THE EVIDENCE SAYS THIS MAY BE THE NORMAL CASE. The only capture of  |
     * | an in-match frame - out/work/gpudump_arena/inst0/state.json - has   |
     * | `oam_main` EMPTY and all nineteen sprites on `oam_sub` ... That     |
     * | capture is a single frame of a single-player replay, so it does not |
     * | settle what a lobby draws.                                          |
     * ======================================================================
     *
     * "THE ONLY CAPTURE" WAS ONE FILE OUT OF 286,097. out/work holds that many
     * per-frame state.json files; the reasoning above had read the one
     * summary state.json at the top of a capture directory and taken it for
     * the whole session. Sampling every 25th frame across all 85 capture
     * streams: 6,229 of 10,931 frames have the main engine's OBJ layer
     * ENABLED, and 5,332 carry sprites in palette 4..7 - in lobby captures
     * (gpudump_lobbywait), in-level walks (gpudump_walk_jabba, _walk_mos) and
     * a live session alike.
     *
     * So "the top screen is a tile layer or a 3D quad and no OAM rewrite can
     * reach it" is NOT established, and neither is the opposite: what the
     * captures do not record is which physical screen the main engine drives,
     * and the four-cell portrait column found on it looks like the BOTTOM
     * screen's. That is why the guard above declines instead of switching
     * engines. This line stays, and it now means what it says rather than
     * standing in for a conclusion.
     *
     * DO NOT "FIX" THIS BY POINTING IT AT THE SUB ENGINE. The nineteen sprites
     * there are the bottom screen's slot column, and the match HUD's read_rows() derives
     * WHICH PLAYER each row belongs to from exactly the palette bank this code
     * would overwrite. Recolouring sub OAM would not move the top-screen ring
     * and would corrupt the identification the mirrored column depends on. */
    if (candidates == 0 && !g_said_empty) {
        g_said_empty = true;
        std::fprintf(stderr,
                     "[fork] slot colour: local slot %d wants palette %d, but "
                     "the MAIN engine's OAM has no sprite in the player-colour "
                     "range - the top-screen ring is not an OAM sprite in this "
                     "frame. See the note in slotcolour.cpp; do not repoint "
                     "this at the sub engine.\n",
                     slot, lsw2_slot_palette(slot));
    }
}
