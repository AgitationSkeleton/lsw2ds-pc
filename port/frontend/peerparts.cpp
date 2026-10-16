/* See peerparts.h. */

#include "peerparts.h"
#include "vwh.h"

#include "cheats.h"

#include "customization.h"
#include "forksession.h"
#include "ui.h"

#include "state.h"

#include <cstdio>
#include <cstring>

namespace {

/* Slot n adds n * kStride; the character is at the base and the five
 * parts follow it as halfwords. */
/* THE APPEARANCE RECORD ARRAY - see cheats.h for kLsw2AppearanceRecord and
 * kLsw2AppearanceStride. Both were declared here too, as kLsw2AppearanceRecord and
 * kLsw2AppearanceStride, and again in the other of these two files: one array, three
 * files, two spellings each. */
constexpr uint32_t kFirstPart = 2u;          /* character, then the parts */
constexpr int kSlots = 4;

constexpr uint32_t kRamLow = 0x02000000u;
constexpr uint32_t kRamHigh = 0x02400000u;

uint16_t g_sent[kLsw2PeerParts] = {0};
bool g_have_sent = false;

void write_halfword(uint32_t a, uint16_t v) {
    /* The bus offers 32- and 16-bit writes; a halfword at an even address is
     * exactly what these fields are, so no read-modify-write is needed - and
     * doing one anyway would risk clobbering the neighbouring part on a
     * misaligned assumption. */
    bus_device_write16(0, a, v);
}

}  // namespace

void lsw2_peerparts_tick() {
    /* LOCKSTEP (vwh.h lsw2_vwh_lockstep): appearance requests from one console are a divergence. */
    if (lsw2_vwh_lockstep()) return;
    if (!ui_fork_build()) return;

    Lsw2ForkSession& session = lsw2_fork_session();
    if (session.state() == Lsw2SessionState::Offline) return;

    /* ---- outbound: our own parts, when they change ---------------------- */
    const uint16_t* mine = lsw2_customization_saved();
    if (mine) {
        /* FIVE OF THE SIX. The local block holds six and the per-player record
         * holds five; the sixth is unsettled, so it stays home
         * rather than travelling as a guess. */
        if (!g_have_sent ||
            std::memcmp(g_sent, mine, sizeof g_sent) != 0) {
            std::memcpy(g_sent, mine, sizeof g_sent);
            g_have_sent = true;
            session.publish_parts(g_sent, kLsw2PeerParts);
        }
    }

    /* ---- inbound: whatever arrived, into the sender's own slot ---------- */
    int station = -1;
    uint16_t parts[kLsw2PeerParts];
    while (session.take_parts(&station, parts, kLsw2PeerParts)) {
        if (station < 0 || station >= kSlots) continue;

        const uint32_t rec = kLsw2AppearanceRecord + uint32_t(station) * kLsw2AppearanceStride;
        if (rec < kRamLow || rec >= kRamHigh) continue;

        for (int i = 0; i < kLsw2PeerParts; ++i)
            write_halfword(rec + kFirstPart + uint32_t(i) * 2u, parts[i]);

        /* THE CHARACTER IS NOT WRITTEN HERE. It travels on every block already
         * (the record header's byte 6) and the roster rules own it; writing it
         * from two places would let a stale parts record undo a character
         * change that arrived a frame later. */
        /* AND ASK THE GAME TO WEAR THEM.
         *
         * Customization did not follow a character switch, and the same cause
         * sits here. This wrote the appearance
         * record and stopped - but `0x020897F8` only applies a record whose
         * GATE byte is non-zero, so five correct halfwords sat in memory being
         * ignored every frame. See cheats.h for the derivation. */
        const bool asked = lsw2_cheats_request_appearance(station);

        std::fprintf(stderr,
                     "[fork] station %d parts -> %u %u %u %u %u%s\n", station,
                     parts[0], parts[1], parts[2], parts[3], parts[4],
                     asked ? "" : "  (no character in that slot yet)");
    }
}
