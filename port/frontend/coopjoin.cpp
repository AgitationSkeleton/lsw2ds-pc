/* See coopjoin.h. */

#include "coopjoin.h"
#include "statelog.h"

#include "coophosts.h"
#include "whsession.h"
#include "forksession.h"
#include "ui.h"

#include "state.h"

#include <cstdio>
#include <cstring>

namespace {

/* The list's connect - vtable slot 0x10 on the wireless manager. */
constexpr uint32_t kWirelessConnect = 0x020ADBCCu;

/* The byte the cartridge's own menus read to know a session is up.
 * The session-start address itself is coopjoin.h's kLsw2WirelessStart, shared
 * with whsession.cpp - see the note there. */
/* See coopjoin.h. The field was folded into the address here, which is
 * what hid that diagnostics.cpp held the same word. */
constexpr uint32_t kWirelessUp = 0x06u;

/* How long a published row lives without a refresh: coophosts.h's
 * kLsw2HostRowTtl, shared with the host-level WH's browse row (vwh.cpp). */
constexpr uint32_t kRowTtl = kLsw2HostRowTtl;

int g_rows = 0;          /* how many we last published */
bool g_on_list = false;  /* the list screen was up last frame */

/* The "Browsing Wireless Game" state id is kLsw2BrowseState in coopjoin.h now -
 * whsession.cpp needs the same number for the fork's host/join role switch, and
 * dup_constants flagged the two copies. */

/* ONE DEREFERENCE TOO MANY, CONFIRMED BY MEASUREMENT.
 *
 * This read `read32(kLsw2StateManager)` first and then added the offset.
 * `kLsw2StateManager` is the manager's BASE ADDRESS, so that is one
 * indirection too many. Measured in a live run, both chains side by side:
 *
 *     base+off    -> 021FDFB8 (id 0F)     a real state object
 *     deref-first -> mgr 021DE298 then 00000004
 *
 * `00000004` is not a pointer, so the range check below always failed and this
 * function returned -1 on EVERY call since it was written. Whatever it guards
 * has never run. */
int running_state_id() {
    const uint32_t st = lsw2_state_current_object();
    if (!st) return -1;
    return int(bus_device_read32(0, st + kLsw2StateId) & 0xFFu);
}

uint8_t read_byte(uint32_t a) {
    return uint8_t((bus_device_read32(0, a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu);
}

void write_byte(uint32_t a, uint8_t v) {
    const uint32_t base = a & ~3u;
    const uint32_t sh = (a & 3u) * 8u;
    const uint32_t w = bus_device_read32(0, base);
    bus_device_write32(0, base, (w & ~(0xFFu << sh)) | (uint32_t(v) << sh));
}

/* THE OVERRIDE. An override REPLACES, and here that is the intent - see the
 * header for why nothing that works is lost. */
void wireless_connect() {
    const uint32_t mode = g_cpu.R[1];
    const uint32_t record = g_cpu.R[2];

    const int row = lsw2_coopjoin_row_for(record);
    if (row >= 0) {
        Lsw2ForkSession& session = lsw2_fork_session();
        const bool ok = session.join_row(row);
        std::fprintf(stderr,
                     "[coop] list row %d chosen (mode %u) - join %s\n",
                     row, unsigned(mode), ok ? "started" : "REFUSED");
        /* NON-ZERO MOVES THE SCREEN ON. The caller does
         * `if (result) [this+0x8C] = 9` - the "connected, waiting for host"
         * phase - and anything else drops it into the failure phase with
         * "Could not connect to host!". Reporting the truth here is what makes
         * a refused join say so on screen instead of hanging. */
        g_cpu.R[0] = ok ? 1u : 0u;
    } else {
        /* NOT ONE OF OURS. Nothing else in this port can complete a wireless
         * connection, so failing is both honest and what would happen anyway -
         * and the screen has a phase for it. */
        std::fprintf(stderr,
                     "[coop] connect for an unpublished record %08X - refused\n",
                     unsigned(record));
        g_cpu.R[0] = 0u;
    }

    g_cpu.R[15] = g_cpu.R[14];
}

/* THE GAME HALF OF BASELINE HOSTING: the baseline HOSTS.
 *
 * Reading pause row 1 - "Host Wireless" - found `0x020B10A8` is the
 * cartridge's own "start a wireless session": it sets `+0x06` on the
 * player-count owner object, a save flag, and calls into the WM layer. The row is already there,
 * already labelled, already reachable from the pause menu in the baseline. So
 * the master-server lobby list's second entry point and the game half of
 * baseline hosting are the same function.
 *
 * Replaced for the same reason the connect is: everything it really does ends in
 * wireless this port does not run. What it should do instead is open the fork's
 * own lobby - and then the row renames itself, because the pause menu's builder
 * reads the very byte this sets. */
void wireless_start() {
    Lsw2ForkSession& session = lsw2_fork_session();
    session.start_lobby();

    /* THE BYTE THE CARTRIDGE WOULD HAVE SET, and it is not decoration: the pause
     * menu builder reads `[owner + 0x06]` to decide whether row 1 says "Host
     * Wireless" or "Shut Off Wireless", and Quit Game's leave path checks it
     * too. Setting it is what makes the game's own menus tell the truth
     * about a session the fork is running. */
    const uint32_t owner = bus_device_read32(0, (kLsw2WirelessOwner + kLsw2WirelessOwnerField));

    /* EXCEPT WHEN THE REAL SESSION IS BEING BROUGHT UP, because this byte is
     * read as a REFUSAL as well as a label.
     *
     * FUN_0208EA58 - state 54's wireless branch, and the cartridge's only
     * route to 0x020B10A8 - opens with
     *
     *     if (*(char *)(*(int *)(owner + 3000) + 6) == 0) { ... start it ... }
     *
     * and 3000 is kLsw2WirelessOwnerField, 6 is this offset. Setting it to 1
     * so the pause menu reads "Shut Off Wireless" also tells the game a
     * session is already running, so it never starts one. The label was worth
     * having while the fork ran its own transport beside the game; it is
     * exactly wrong while the game is being asked to run its own. */
    if (owner >= 0x02000000u && owner < 0x02400000u && !lsw2_wh_enabled())
        write_byte(owner + kWirelessUp, 1);

    std::fprintf(stderr, "[coop] Host Wireless -> the fork's lobby is up\n");
    g_cpu.R[15] = g_cpu.R[14];
}

const NdsDispatchEntry kCoopOverrides[] = {
    { kWirelessConnect, 0, wireless_connect, nullptr },
    { kLsw2WirelessStart, 0, wireless_start, nullptr },
};

}  // namespace

int lsw2_coopjoin_row_for(uint32_t record) {
    if (record < kLsw2HostTable) return -1;
    const uint32_t off = record - kLsw2HostTable;
    if (off % kLsw2HostStride != 0) return -1;      /* not a record boundary */
    const int row = int(off / kLsw2HostStride);
    if (row >= kLsw2HostSlots || row >= g_rows) return -1;
    return row;
}

void lsw2_coopjoin_install(bool fork_build) {
    /* THE BASELINE ONLY. The fork draws its own browser and this would put
     * a second, different list in front of the same player.
     *
     * THE FLAG IS A PARAMETER BECAUSE ui_fork_build() CANNOT ANSWER HERE, and
     * this guard used to ask it. dispatch_overrides.cpp says so at the top of
     * the very function that calls this: registration runs from main(), before
     * the title banks and therefore before the shell exists, so
     * `ui_fork_build()` is FALSE at this moment in BOTH executables. The guard
     * was inert.
     *
     * That inversion is the whole of the bug this fixed. The call also sat
     * AFTER `if (!core) return;`, which outside LSW2_HLE means fork-only - so
     * an override documented as "the baseline only" installed in the FORK,
     * where the comment above says it must not, and never in the baseline,
     * where the master-server lobby list needs it. Both halves wrong, from one
     * flag that reads false too early.
     *
     * The caller derives `fork` from the environment for exactly this reason
     * and now passes it in. */
    if (fork_build) return;
    nds_register_dispatch(NDS_ARM9, kCoopOverrides,
                          unsigned(sizeof kCoopOverrides / sizeof *kCoopOverrides),
                          0xFFFF0000u);
    std::fprintf(stderr, "[coop] wireless connect redirected to the fork's join\n");
}

void lsw2_coopjoin_publish() {
    if (ui_fork_build()) return;

    Lsw2ForkSession& session = lsw2_fork_session();

    /* FETCH WHEN THE SCREEN OPENS, ONCE.
     *
     * The directory refresh is a blocking HTTP request; doing it per frame would
     * turn the list into a slideshow and hammer the server. The list screen is
     * state 0x22, so its arrival is the moment to look - the same
     * "fetch before it is shown" rule `open_master_list()` follows, for the same
     * reason: a list that fills in under the cursor moves the row the player was
     * about to choose. */
    const int state = running_state_id();
    if (state != kLsw2BrowseState) {
        g_on_list = false;
        return;                       /* not our screen; leave the table alone */
    }
    if (!g_on_list) {
        g_on_list = true;
        session.refresh_lobbies(/*lan_first=*/true);
        std::fprintf(stderr, "[coop] lobby list opened - %d lobbies\n",
                     session.known_lobbies());
    }
    const int have = session.known_lobbies();
    const int rows = lsw2_coophosts_rows(have);

    if (rows < have) {
        static bool said = false;
        if (!said) {
            said = true;
            /* NO SILENT TRUNCATION. Sixteen of twenty shown as though it were
             * everything is the quiet failure this project keeps warning about. */
            std::fprintf(stderr,
                         "[coop] %d lobbies, but the game's list holds %d\n",
                         have, kLsw2HostSlots);
        }
    }

    for (int i = 0; i < kLsw2HostSlots; ++i) {
        const uint32_t rec = kLsw2HostTable + uint32_t(i) * kLsw2HostStride;

        if (i >= rows) {
            /* AN EMPTY SLOT IS A ZERO TTL, which is exactly how the cartridge
             * marks one - the count getter tests this very field. Clearing the
             * name too would be tidier and is deliberately not done: the game
             * never reads a name whose TTL is zero, and writing 30 bytes per
             * empty slot every frame to satisfy tidiness is work for nobody. */
            bus_device_write32(0, rec + kLsw2HostTtl, 0);
            continue;
        }

        char name[kLsw2HostNameMax + 1];
        session.lobby_name(i, name, int(sizeof name));

        for (int c = 0; c < kLsw2HostNameMax; ++c)
            write_byte(rec + kLsw2HostName + uint32_t(c),
                       uint8_t(c < int(std::strlen(name)) ? name[c] : 0));

        bus_device_write16(0, rec + kLsw2HostStatus, lsw2_coophosts_status());
        bus_device_write32(0, rec + kLsw2HostTtl, kRowTtl);
    }

    g_rows = rows;
}
