/* THE HOST-LEVEL WH, GUEST SIDE: the game's wireless manager answered by the PC.
 *
 * port/net/lsw2_vwh.h is the data-sharing session over a socket. This file binds that session to the
 * cartridge: it replaces the wireless manager's methods that reach into WM
 * (vtable 0x020FFD50 slots 0,1,2,3,4,5,6,8,9,15,16,17,19,20,24) and keeps the
 * WH context fields the rest of the cartridge reads coherent in guest memory -
 * the system state (+0x20, read by WH_GetSystemState over a direct BL), the
 * last error (+0x30), the connected bitmap (+0x0E), this console's AID (+0x08),
 * the channel (+0x02) and the host table - so the pure accessors (slots 7, 10,
 * 11, 12, 13, 14, 18, 21, 22, 23, 25) keep running as the cartridge's own code.
 *
 * Above it the game is untouched: its host and join screens, the join prompt,
 * the netgame, the rules chooser, the statue select and the match all run on
 * datasets exactly as they do over the radio - without the radio.
 *
 * The fork's default since it carried four consoles at 60 fps; LSW2_VWH=0 turns it
 * off, and the baseline never has it (see lsw2_vwh_enabled in lsw2_net_bind).
 * With it off, nothing here is registered and the radio route is unchanged. */
#ifndef LSW2_VWH_GUEST_H
#define LSW2_VWH_GUEST_H

#include <cstdint>

struct NdsDispatchEntry;

/* The override table to register, or null (count 0) when the mode is off.
 * Registered BEFORE the core tables so these answers win over the older
 * observe-and-forward hooks on the same methods. */
const NdsDispatchEntry* lsw2_vwh_overrides(unsigned* count);

/* Once a frame from the fork tick: a connecting child becomes DATASHARING when
 * its session goes live, a lost session ends the guest's, and the host table
 * the browse list reads is refreshed while scanning. */
void lsw2_vwh_tick();

/* TRUE WHILE THIS CONSOLE IS IN LOCKSTEP WITH ANOTHER - a host-level WH
 * session is live, the game is data-sharing, and the last dataset had at least
 * two members.
 *
 * THE RULE IT ENFORCES: while this holds, the fork must not change
 * simulation state from the between-frames tick. Each console's tick runs at a
 * different point in the game's own frame, so a write made there lands at a
 * different simulation step on every machine, and the cartridge's cross-player
 * check ends the match (state 38) a second later. Measured: the roster sync
 * swapping a peer's actor two steps apart on the two consoles. Visual-only
 * writes are fine; a write that changes what the game computes must either
 * wait for the session to end or happen at a dataset boundary inside the
 * exchange (vwh.cpp, the dataset boundary in vwh_step_ds), where every console applies it at the
 * same step. */
bool lsw2_vwh_lockstep();

/* HOST: a joiner is connected and waiting to be seated, or being seated right
 * now (the game's prompt and lobby reload). The round must not start under it:
 * the lobby countdown and Start Game hold until it is false (fork.cpp). */
bool lsw2_vwh_seating();

/* This console hosts a live host-level WH session. */
bool lsw2_vwh_hosting();

/* This console's AID in the live session, or -1 when it has none yet. */
int lsw2_vwh_local_aid();

/* The game's multi-view bit (0x40 of 0x027E02A8) - set once a lobby has seated
 * a player; while it is set, the cartridge's per-player actions (customize,
 * leave) travel as netgame messages instead of acting locally. */
bool lsw2_vwh_multiview();

/* EFFECTS - the dataset boundary's way in (lsw2_vwh.h has the channel).
 *
 * A change the fork must make to the simulation while consoles share datasets
 * is not made by the console that decides it. The HOST posts it here; the
 * session stamps it on the next commit it can fit it in; and every console,
 * the host included, applies it in vwh_step_ds when it consumes that dataset,
 * before the game sees the dataset - the same bytes at the same step
 * everywhere. Host only: false on a client, when no session is up, or when the
 * record cannot be queued. */
constexpr uint8_t kLsw2EffectLobbyReturn = 1;   /* no payload - scorescreen.h */
/* {u8 depth}: the session's pipeline depth rises to fit a joiner's link, on
 * every console at once (vwh.cpp, seating). */
constexpr uint8_t kLsw2EffectDepth = 2;
/* {u8 rule, u8 skip select, u32 salt, u16 character[4]}: the round's roster -
 * sessionroster.h. */
constexpr uint8_t kLsw2EffectRoster = 3;
/* {u8 station}: TEST ONLY - LSW2_TEST_KILL on the host. That station's actor
 * dies at this dataset on every console, so a drive can reach the respawn (and
 * a Random rule's re-roll) without scripting combat - sessionroster.h. */
constexpr uint8_t kLsw2EffectTestKill = 4;
/* No payload: the ALL rule's Free Play pick is over, and every console asks
 * for the round start it stood in for at this dataset - freeplaypick.h. */
constexpr uint8_t kLsw2EffectPickEnd = 5;
/* {u8 station, s32 x, s32 y, s32 z (20.12)}: TEST ONLY - LSW2_TEST_TELEPORT on
 * the host. That station's actor is placed there at this dataset on every
 * console, so a drive can reach a spot in an arena without steering the tank
 * camera across it - sessionroster.h. */
constexpr uint8_t kLsw2EffectTestTeleport = 6;
/* {u8 station, u8 attacker station, u8 damage type}: TEST ONLY - LSW2_TEST_HIT
 * on the host. That station's actor takes one hit from the attacker's through
 * the game's own hit handler at this dataset on every console, so a drive can
 * reach what a hit does (e.g. a hat knocked off) without aiming a
 * shot - sessionroster.h. */
constexpr uint8_t kLsw2EffectTestHit = 7;
bool lsw2_vwh_post_effect(uint8_t type, const uint8_t* payload, int len);

/* For the frontend's pacing: committed datasets this console holds but has not
 * consumed. A client that falls behind the host catches up by running frames
 * unthrottled while this is above a couple. 0 when not a live client. */
int lsw2_vwh_backlog();

/* Which VBlank period the guest is in - the index steps as each VBlank begins -
 * from frontend.cpp, which owns the frame length. The host-level WH hands out
 * at most one dataset per console per VBlank - WM's one MP cycle a frame -
 * because the netgame's tick loops on the exchange until it reports "no
 * dataset". Keyed to the VBlank and not to line 0 because the game
 * exchanges right after its VBlank wait (see vwh.cpp). */
uint64_t lsw2_vblank_ordinal();

#endif /* LSW2_VWH_GUEST_H */
