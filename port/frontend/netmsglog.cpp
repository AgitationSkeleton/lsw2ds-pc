/* The guest half of netmsglog.h: walk each station's slots, name what is new. */

#include "netmsglog.h"

#include "freeplaypick.h"
#include "ui.h"
#include "viewmask.h"

#include "state.h"

#include <cstdio>

namespace {

uint32_t guest_read32(uint32_t addr, void*) {
    return bus_device_read32(0, addr);
}

/* The last sequence reported per (station, slot), mirroring the game's own
 * dedup at 0x020B0488 - it dispatches only when the sequence differs from what
 * it last saw. Without this the log would name the same message every frame it
 * sits in the buffer. -1 means "nothing seen here yet". */
int g_last_seq[kLsw2MsgStations][16];
bool g_init = false;

int g_station = 0;   /* which station the current walk is reporting */

/* WHICH VISIT THIS IS, because the game's own record is indexed by VISIT ORDER
 * and not by slot id. The pump walks the list with `SUB r6,r1,#1` before the
 * loop and `ADD r6,r6,#1` inside it, so r6 counts entries VISITED - a message
 * in slot 8 reached on the second visit is recorded at index 1, not 8. That
 * was checked against a live console: the record read {59, 60, ...} while the
 * two messages were slot 0 seq 59 and slot 8 seq 60. */
int g_visit = 0;

void report(int slot, int type, int seq, void*) {
    const int visit = g_visit++;
    if (slot < 0 || slot >= 16) return;
    if (g_last_seq[g_station][slot] == seq) return;
    g_last_seq[g_station][slot] = seq;

    /* THE ALL RULE'S PICK COUNTS THE SWITCHES (freeplaypick.h): a message 7
     * from a station is that player confirming on the grid. */
    lsw2_freeplaypick_note_message(g_station, type);

    /* AND WHAT THE GAME HAS ACTUALLY CONSUMED, which is a different thing from
     * what is sitting in the buffer.
     *
     * The goal this serves, in plain english: the match-start seed message was
     * seen in BOTH consoles' buffers and never acted on, and establishing that
     * took a hand-read of the game's own record after the failure. Printing it
     * here puts "queued" and "handled" side by side on every message, so a
     * message that is never picked up says so the moment it happens instead of
     * needing a post-mortem.
     *
     * The record is the pump's own, at `owner + station*10 + 0x40 + visit`
     * (`MLA r1,r5,r0,r9` with r0 = 10), and the pump dispatches only when it
     * DIFFERS from the message's sequence - so `handled` equal to the sequence
     * means this message has been through the handler, and anything else means
     * it has not, yet. */
    const uint32_t rec = kLsw2MsgOwner + uint32_t(g_station) * 10u +
                         kLsw2MsgSeqBase + uint32_t(visit);
    const uint32_t word = bus_device_read32(0, rec & ~3u);
    const int handled = int((word >> ((rec & 3u) * 8u)) & 0xFFu);

    std::fprintf(stderr,
                 "[fork] net message: station %d slot %2d type 0x%02X seq %d"
                 "  handled=%d%s\n",
                 g_station, slot, type, seq, handled,
                 handled == (seq & 0xFF) ? "" : "  <- NOT YET HANDLED");
}

}  // namespace

void lsw2_netmsglog_tick() {
    if (!ui_fork_build()) return;

    if (!g_init) {
        for (int s = 0; s < kLsw2MsgStations; ++s)
            for (int i = 0; i < 16; ++i) g_last_seq[s][i] = -1;
        g_init = true;
    }

    /* ONLY WHILE THE GAME BELIEVES THERE ARE PLAYERS. The pump itself is
     * bounded by the same count, so walking these slots when the
     * mask is empty would be reading a buffer nothing is filling. It also keeps
     * the log quiet in single player, which is most of the time. */
    const int mask = lsw2_viewmask_guest();
    if (mask <= 0) return;
    const int seated = lsw2_viewmask_count(unsigned(mask), kLsw2ViewRecords);
    if (seated < 2) return;

    for (int s = 0; s < kLsw2MsgStations && s < seated; ++s) {
        g_station = s;
        g_visit = 0;
        const uint32_t base = kLsw2MsgOwner +
                              uint32_t(s) * kLsw2MsgStationStride +
                              kLsw2MsgSlotBase;
        lsw2_netmsg_walk(base, guest_read32, nullptr, report);
    }
}
