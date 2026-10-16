/* Where the guest meets the session.
 *
 * The lockstep session (lsw2_lockstep.h) deliberately knows nothing about the
 * ARM9 - that is what makes it testable with four stations in one process and
 * no ROM. The dispatch overrides deliberately know nothing about sockets. This
 * header is the one place the two are allowed to see each other, and it is
 * three functions wide on purpose.
 *
 * THE FORK SESSION OWNS THE SESSIONS. The fork's host and join flows
 * (frontend/forksession.cpp) create them and hand them over here; whenever
 * there is no networked game the accessor returns null, and the overrides
 * treat that exactly as the guest treats a frame with no data - see
 * lsw2_net_bind.cpp for why that is the correct answer rather than a
 * placeholder.
 */

#ifndef LSW2_NET_BIND_H
#define LSW2_NET_BIND_H

class Lsw2LockstepSession;

/* The session the guest's data-sharing calls should run through, or null when
 * there is no networked game. Null is a normal state, not an error. */
Lsw2LockstepSession* lsw2_net_active_session();

/* Which station index this machine is. The guest calls it an AID, and the two
 * are the same number - the connected-station bitmap the game builds is
 * indexed by AID, and the session's station indices are the
 * bits of that bitmap. */
int lsw2_net_local_station();

/* Handing over a session transfers nothing: the caller keeps ownership and must
 * clear this before destroying it. Passing null ends the guest binding, which
 * is what a disconnect does. */
void lsw2_net_set_active_session(Lsw2LockstepSession* session, int local_station);

/* THE HOST-LEVEL WH (lsw2_vwh.h). Whether the game's wireless manager is answered
 * by the PC session instead of the emulated radio: on in the fork (LSW2_FORK=1),
 * off in the baseline, and LSW2_VWH=0/1 overrides either. Read once.
 * Guest-free so the fork session and the guest binding can both ask it. */
class Lsw2VwhSession;
bool lsw2_vwh_enabled();
/* The fork session hands its host-level WH session over the same way, keeping
 * ownership; null when there is none. `host_name` is the name the joiner's
 * browse list shows for the host it is connected to. */
Lsw2VwhSession* lsw2_vwh_session();
void lsw2_vwh_set_session(Lsw2VwhSession* session);
const char* lsw2_vwh_host_name();
void lsw2_vwh_set_host_name(const char* name);

#endif /* LSW2_NET_BIND_H */
