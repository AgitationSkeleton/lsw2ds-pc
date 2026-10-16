/* See lsw2_net_bind.h. Deliberately trivial - the value of this file is that
 * it is the ONLY coupling between the transport and the guest.
 *
 * WHY NULL IS A CORRECT ANSWER AND NOT A STUB.
 *
 * When there is no networked game, the data-sharing overrides report "no data
 * this frame" to the guest. That is not a placeholder standing in for something
 * better later - it is what the shipped game is told whenever the radio has
 * nothing for it, and the game's own handling of it is unremarkable: tracing
 * the path shows the only thing that happens is the errcode gets recorded.
 * No state change, no disconnect, no teardown.
 *
 * So a single-player session with the binding registered behaves as a session
 * whose peers have not sent anything, which is exactly what it is.
 */

#include "lsw2_net_bind.h"

#include <cstdio>
#include <cstdlib>

namespace {
Lsw2LockstepSession* g_session = nullptr;
int g_local_station = 0;
Lsw2VwhSession* g_vwh = nullptr;
char g_vwh_host_name[32] = "Host";
}  // namespace

bool lsw2_vwh_enabled() {
    /* ON IN THE FORK, OFF IN THE BASELINE, and LSW2_VWH decides either way.
     *
     * The host-level WH is the fork's PC networking: four consoles reach and
     * hold the arena on it at 60 fps, which the radio route never did past
     * two. The baseline keeps the radio - its co-op rides it and the baseline
     * stays as the game plays - and the
     * overrides that answer the game's wireless manager are only registered in
     * the fork anyway (dispatch_overrides.cpp). LSW2_FORK is published by the
     * runner from argv[0] before any of this runs.
     *
     * LSW2_VWH=0 is the way back to the radio in the fork; =1 is kept so the
     * harness scripts that name it keep meaning what they say. */
    static const bool on = [] {
        const char* e = std::getenv("LSW2_VWH");
        if (e && *e) return *e == '1';
        const char* fork = std::getenv("LSW2_FORK");
        return fork && *fork == '1';
    }();
    return on;
}

Lsw2VwhSession* lsw2_vwh_session() { return g_vwh; }

void lsw2_vwh_set_session(Lsw2VwhSession* session) { g_vwh = session; }

const char* lsw2_vwh_host_name() { return g_vwh_host_name; }

void lsw2_vwh_set_host_name(const char* name) {
    std::snprintf(g_vwh_host_name, sizeof g_vwh_host_name, "%s",
                  (name && *name) ? name : "Host");
}

Lsw2LockstepSession* lsw2_net_active_session() { return g_session; }

int lsw2_net_local_station() { return g_local_station; }

void lsw2_net_set_active_session(Lsw2LockstepSession* session,
                                 int local_station) {
    g_session = session;
    g_local_station = local_station;
}
