/* The guest-touching half of the roster rule, stubbed for the tests.
 *
 * WHY A STUB AND NOT rosterenforce.cpp. That file reads the live guest through
 * cheats.h, which drags the whole runtime onto a link line for two functions
 * the session only calls to reset a counter. The session's test cares that the
 * reset HAPPENS, not what it resets - the behaviour itself is covered by
 * test_roster.cpp against the pure logic.
 *
 * NAMED test_* FOR A REASON. port/build.sh globs every .cpp in this directory
 * and excludes only test_*, so a file called rosterstub.cpp would have been
 * swept into the real shell alongside rosterenforce.cpp - two definitions of
 * the same two functions, and the stub might have won. The first version of
 * this file was called that, and its own comment predicted the failure it was
 * about to cause.
 */

#include "roster.h"

void lsw2_roster_enforce() {}
void lsw2_roster_publish() {}
void lsw2_roster_match_state() {}
void lsw2_roster_sync_peers() {}
void lsw2_roster_enforce_reset() {}

/* rngsync's guest half, stubbed for the same reason. */
#include "rngsync.h"
void lsw2_rng_apply_seed(uint32_t) {}
void lsw2_rng_sync_tick() {}

/* THE CARTRIDGE'S WIRELESS SESSION, stubbed for the same reason: it reaches the
 * guest CPU, which does not exist in a host test binary. Returning false is the
 * honest answer here - it is exactly what the real function returns when the
 * real-WH route is off, so the menu tests exercise the fork's own join path,
 * which is the path they were written to check. */
#include "whsession.h"
bool lsw2_wh_console_join() { return false; }
void lsw2_wh_console_open_lobby() {}
void lsw2_wh_set_role(bool) {}
void lsw2_wh_set_force_browse(bool) {}
/* "Shut Down Lobby" asks the guest-side hop to run the cartridge's session end; the host
 * unit binary has no guest, so the request is a no-op here. */
void lsw2_wh_request_session_end(const char*) {}

/* The cartridge end-of-session, stubbed for the host tests: it reaches the
 * guest CPU, which a host binary does not have. Doing nothing matches what the
 * real one does when no wireless session is up - the state every test is in. */
void lsw2_wh_end_session() {}

/* The lobby respawn (lobbyrespawn.h), stubbed for the same reason: it reads and writes the
 * guest's state manager through the bus, and a host test binary has no bus.
 *
 * Returning false is the honest answer rather than a convenient one - it is
 * exactly what the real function returns when the experiment is disarmed, which
 * is its default and therefore the state every test is in. The session test
 * cares that start_lobby() reaches this call; what it does to the guest is
 * measured on a running console, not here. */
#include "lobbyrespawn.h"
bool lsw2_lobby_respawn_armed() { return false; }
void lsw2_lobby_respawn_arm(const char*) {}
void lsw2_lobby_respawn_tick() {}
void lsw2_lobby_seat_peers_tick() {}
