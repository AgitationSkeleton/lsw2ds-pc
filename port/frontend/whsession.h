/* RUNNING THE CARTRIDGE'S OWN WIRELESS SESSION, with the radio stood in for.
 *
 * The goal is TCP/IP play kept in sync across machines without drifting
 * apart, and in practice that means the players are genuinely connected
 * through the game's real arena multiplayer system, playing an actual
 * deathmatch or CTF round on one of the two arenas (Jabba's Tower, Mos Eisley
 * Streets) - not just warped onto the map side by side.
 *
 * Measurement showed why the warp is not that: two machines on level 4 with the
 * view manager null and the pump gate idle are two single-player arenas.
 * The real flow was then traced, and it has a name.
 *
 * THE LAYER IS NINTENDO'S WH, SHIPPED UNMODIFIED IN THE CARTRIDGE. The table at
 * 0x020FFD20 spells its states out - WH_SYSSTATE_STOP, IDLE, SCANNING, BUSY,
 * CONNECTED, DATASHARING, KEYSHARING, MEASURECHANNEL, DISCONNECTED, ERROR -
 * and the fork's reads 0. STOP. It has never started.
 *
 * SO THIS IS A BRING-UP, NOT A REIMPLEMENTATION. The plan is
 * to run the cartridge's own session start and stand in only where it reaches
 * the radio. Two pieces, and they are deliberately separate:
 *
 *   THE SESSION START ITSELF. `0x020B10A8` is the cartridge's "start a wireless
 *   session" and coopjoin.cpp REPLACES it, keeping one byte out of five
 *   statements. The first statement it drops, FUN_0207281C, is the one that
 *   builds the view manager everything else needs. So the fork asks for the
 *   real function to run, through the ordinary guest-call seam.
 *
 *   THE READINESS POLL. The session start spins on vtable slot 0 -
 *   `while (ready() == 0) wait();` - and that bottoms out in FUN_020D61C8, in
 *   the 0x020D6xxx WM range, which is the actual radio. That spin is why the
 *   port could never let this function run: with no hardware it never ends.
 *   Answering it is what lets the seam make the call at all - a deferred drain
 *   on a dispatch override was built for this first and abandoned, because the
 *   override it hung on fires ONCE in a whole run.
 *
 * WHAT THE OVERRIDE DOES, AND WHAT IT DOES NOT. It calls the cartridge's own
 * poll by symbol first, so the two buffers that function allocates - 0xC80 and
 * 0x220 - are allocated by the game, in the game's heap, exactly as the game
 * would. Only its ANSWER is substituted, and only when the real one says "not
 * ready", which without a radio it always will. Nothing is reimplemented; one
 * boolean is overruled.
 *
 * ON BY DEFAULT; set LSW2_REAL_WH=0 to switch it off. It was once off unless
 * LSW2_REAL_WH=1, gated on a drive showing the state machine advancing and the
 * guest healthy - and the gate stayed shut after that drive had passed. That
 * mattered because the fork's real-flow round start is downstream of this
 * flag, so every ordinary launch was taking the warp described above.
 *
 * The opt-out is kept because a bad bring-up is known to damage the guest,
 * and a thing that cannot be switched off cannot be bisected.
 */

#ifndef LSW2_WHSESSION_H
#define LSW2_WHSESSION_H

#include "enhanced.h"   /* kLsw2LocalCamera */

#include <cstdint>

/* Whether the bring-up is switched on. Read once from LSW2_REAL_WH. Exposed
 * because the dispatch table has to decide whether to register the readiness
 * override at startup, long before anything else here runs. */
bool lsw2_wh_enabled();

/* WHY THERE IS NO CHOOSER DRIVE HERE, and it is the most useful thing this
 * file has established.
 *
 * The route is right: state 54 with `[state + 0xEC] = 1` is the cartridge's
 * own way into a wireless session. Two ways of setting that field were built
 * and both were correct about the field:
 *
 *   FROM THE FORK TICK - fired in order and landed one tick late, because the
 *   state is built AND updated on the guest's thread between two ticks;
 *
 *   FROM THE STATE'S OWN UPDATE - FUN_0208E514, whose only reference in the
 *   whole image is the vtable slot at 0x020FF530, so it is virtual and
 *   interceptable. The override never fired.
 *
 * IT NEVER FIRED BECAUSE THE STATE DIES BEFORE ITS FIRST UPDATE. The log is
 * unambiguous:
 *
 *     [state] entered 54   trail: -1 -> 15 -> 48 -> 54
 *     [bus] ARM9 unmapped read 0xE92D500C
 *     [bus] ARM9 unmapped read 0xE3D4C004
 *
 * and those "addresses" are ARM instruction encodings - 0xE92D500C is
 * `stmdb sp!,{r2,r3,r12,lr}`. The guest is executing memory that is not code,
 * immediately on entry.
 *
 * SO IT IS NOT A WIRING PROBLEM. An earlier investigation reached the same wall
 * from the other side and guessed at it: state 54 expects the state it came
 * from to have prepared things a warped-in level never did. That is now the
 * reading with evidence behind it. The trail says `15 -> 48 -> 54`; state 48 is
 * where the fork's COLD WARP into the arena lobby leaves the game, and the cartridge's own route reaches 54 from its
 * menu chain. A state built to sit on top of a menu context cannot be dropped
 * on top of a level the fork teleported into.
 *
 * WHICH IS A STATEMENT ABOUT THE FORK'S ARCHITECTURE, not about this file. The
 * fork cold-warps into the arena lobby and then runs its own transport beside
 * the game; the game's own path to a wireless match walks its menu states. The
 * two have to be reconciled before any of the WH work can land, and that is a
 * design question rather than another override. */

/* ASK THE GAME FOR THE CHOOSER, PURELY TO WATCH IT DIE.
 *
 * This is a PROBE, not a feature. State 54 is the cartridge's own route into a
 * wireless session and it dies on entry from the fork's lobby:
 *
 *     [state] entered 54   trail: -1 -> 15 -> 48 -> 54
 *     [bus] ARM9 unmapped read 0xE92D500C
 *
 * and 0xE92D500C is the encoding of `stmdb sp!,{r2,r3,r12,lr}` - the guest
 * executing memory that is not code. Three routes to setting the state's
 * selection were built and abandoned because none of them ever got that far;
 * what has never been done is looking at WHERE the jump comes from.
 *
 * The framework keeps a branch trace ring and the debug surface exposes it as
 * `runtime_trace`, with the registers at each entry. So: request the state,
 * let it die, and read the last good branch before the wild one. That is one
 * measurement and it replaces a fourth guess.
 *
 * OFF UNLESS LSW2_PROBE_CHOOSER=1, separately from LSW2_REAL_WH, because this
 * one is expected to break the guest and must never be on by accident. */
void lsw2_wh_probe_chooser();

/* STATE 54'S PER-FRAME UPDATE - THE SLOT THE STATE MACHINE ACTUALLY CALLS.
 *
 * An override on 0x0208E514 was built first and never fired once, and the
 * reason was not the state dying (that claim is withdrawn) but the SLOT.
 * 0x0208E514 had been identified as slot 3 of the
 * vtable at 0x020FF524 and that is correct; what nobody checked is whether the
 * machine calls slot 3. It does not. `FUN_0205EF84`, the per-slot tick the
 * state pump runs twice a frame, makes exactly two virtual calls:
 *
 *     (**(code **)(*state + 0x10))()            <- the update
 *     (**(code **)(*state + 0x14))(state, 7|8, 0, 0)
 *
 * and state 54's +0x10 is 0x0208E7E0. Slot 3 is reached from inside that, when
 * the menu is acted on. So the selection has to be written here, before this
 * delegates - which is the first moment in the frame the state's own code has
 * not yet read it.
 *
 * Its only reference in the image is the vtable word at 0x020FF534, so it is
 * called virtually and the dispatch table can intercept it. */
void lsw2_wh_probe_update();

/* PUT THE CHOOSER INTO ITS WIRELESS MODE, once, after state 54 is entered.
 *
 * WHAT THIS IS FOR. The chooser draws four rows - map, gamemode, time limit,
 * Start Game - and pressing Start Game runs FUN_0208EA58, which switches on the
 * object's +0xEC. The constructor leaves that 0, and case 0 just asks for the
 * state in +0xF0 without touching the radio. CASE 1 IS THE ONE THE REAL
 * WIRELESS FLOW NEEDS: it calls the cartridge's own wireless session start FUN_020B10A8 and
 * then requests state 15. Measured: with the field at 0, pressing Start Game
 * over the debug socket changed nothing at all.
 *
 * A PLAIN MEMORY WRITE, not the +0x08 override built to do this. That override
 * on 0x0208E374 has never once printed its line, so it is not being reached -
 * and this project's rule is to verify a hook fires before building on it. The
 * field's meaning is read straight out of the switch, so writing it needs no
 * guest call and has no timing to get wrong. */
void lsw2_wh_probe_select_wireless();

/* Dump the branch ring a few frames after the probe fired. Port-side, because
 * once the guest is wedged the debug socket's `runtime_trace` times out - its
 * pump runs between frames and those have stopped. */
void lsw2_wh_probe_dump_trace();

/* The readiness poll, vtable slot 0 of the WH manager - 0x020AD438. See the
 * header comment for what it substitutes and what it leaves alone. */
void lsw2_wh_ready_poll();



/* MAKE THE VIEW MANAGER EXIST, THE WAY THE CARTRIDGE MAKES IT EXIST.
 *
 * PLAIN ENGLISH, AND WHICH PROJECT GOAL THIS SERVES. Real arena multiplayer
 * needs two consoles inside one real match on an arena map (Jabba's
 * Tower, Mos Eisley Streets) chosen through the game's own map/gamemode screen - menu
 * state 54 - rather than warped into a level side by side. Investigation found
 * the single thing standing in the way: state 54 makes a virtual call through the
 * VIEW MANAGER at 0x021092F4, that pointer is NULL in the fork, and the call
 * lands in the BIOS and takes the frame loop down with it. This function is
 * the missing step. Nothing here is invented: it runs the cartridge's own
 * allocator.
 *
 * WHY IT WAS MISSING, WHICH IS NOT WHAT EARLIER NOTES SAID.
 *
 * They recorded that `coopjoin.cpp` REPLACES 0x020B10A8 - the cartridge's
 * wireless session start, whose first act allocates the manager - and that the
 * replacement is why the fork has none. That is wrong for the build it
 * matters in. `lsw2_coopjoin_install` opens with `if (fork_build) return;`, so
 * in `lsw2_bountyhunter.exe` the real session start is present and unhooked.
 * The manager is null for a simpler reason: NOTHING EVER CALLS IT. The probe
 * asks for state 54 directly and skips the step the real game always performs
 * first - a player hosting a wireless game presses "Host Wireless" (which is
 * 0x020B10A8) and only then reaches the map chooser. The order was never
 * circular; the fork just left out the first half.
 *
 * WHAT IT CALLS AND WHY THAT ONE. 0x020B10A8 is four statements:
 *
 *     FUN_0207281c();                        <- allocates the view manager
 *     whmgr->vtable[0x4C](whmgr, 1);            arms the radio
 *     owner[0xC] = 5;  owner[6] = 1;            the session-is-up bytes
 *     FUN_020861bc(x, 0xB);
 *
 * Only the first is wanted here, and it takes no arguments. The other three
 * are about a radio this port does not run, and `owner[6] = 1` is actively
 * harmful: `FUN_0208EA58`, state 54's own wireless branch, treats that byte as
 * "a session is already up" and refuses to start one - the exact trap
 * coopjoin.cpp documents at its `kWirelessUp` write.
 *
 * WHY THIS IS NOT A HOST CALL, WHICH WAS MEASURED RATHER THAN ASSUMED.
 *
 * It was written as one first - `lsw2_guest_call(FUN_0207281C)` from the fork
 * tick - and the machine named the instruction it died on:
 *
 *     [fork] WH state -> 3 WH_SYSSTATE_BUSY
 *     [guestcall] 0207281C did not return: unwinding=0 r15=020ADD94
 *                 active_cpu=ARM9 resumes=4096 events=0  RESUME LIMIT
 *
 * Two things in that are worth keeping. The WH state moved STOP -> BUSY, so
 * the call really did run the cartridge's wireless bring-up; and there were
 * ZERO unmapped reads, so the null-view-manager crash is simply absent from
 * this path.
 * It did not fault - it SPUN. 0x020ADD94 is `cmp r0,#0`, the instruction
 * immediately after `bl 0x020D71C8`, and that callee is
 *
 *     FUN_020d6618(1,2); FUN_020d6418(2,arg); FUN_020d6488(2,0)
 *
 * - the NitroSDK PXI path, i.e. a handshake with the ARM7. The fork tick runs
 * between frames and advances only the ARM9, so the answer it is waiting for
 * can never arrive. This is the same shape as the DMA stall guestcall.h
 * documents, one device class further out.
 *
 * AND THE SEAM CANNOT BE TAUGHT TO FIX IT. Extending the resume loop to run
 * the other core was the obvious next move and the framework forbids it:
 * scheduler.h states that "the call-return stack is empty at every slice
 * boundary ... so only the ArmCpuState is swapped". A host call parked
 * mid-function has a non-empty return stack by definition, so it cannot
 * survive the switch that would let the ARM7 answer. Not a gap - a property.
 *
 * SO THE GUEST RUNS IT, on its own thread, across slice boundaries, where both
 * cores are live. lsw2_wh_trampoline_hop below points the guest's PC at the
 * allocator from inside an override that already fires in the arena lobby, and
 * arranges for it to come back. dispatch_overrides.cpp drains its deferred
 * redraw from that same override for word-for-word the same reason.
 *
 * Returns one of Lsw2ViewManagerResult. EVERY FAILURE IS DISTINCT AND SAID
 * OUT LOUD, because the interesting one is silent: FUN_0207281C guards the
 * constructor with `if (thunk_FUN_0205e540(x, 3, 4))` and stores 0 when that
 * is false, so it can complete perfectly and leave the pointer null. A
 * "worked" that produced nothing is the failure this project keeps being
 * bitten by. */
enum Lsw2ViewManagerResult {
    kLsw2ViewManagerAlready = 0,   /* it was already there */
    kLsw2ViewManagerMade = 1,      /* the allocator ran and built it */
    kLsw2ViewManagerNotNow = -1,   /* asked for; the hop has not happened yet */
    kLsw2ViewManagerStillNull = -3 /* it completed, and the pointer is STILL 0 */
};
int lsw2_wh_make_view_manager();

/* THE HOP ITSELF, called from the top of an override that runs in the arena
 * lobby. `host_entry` is that override's own guest address, which becomes the
 * allocator's return address so the host is entered a second time and its real
 * body still runs.
 *
 * Returns true when it has just redirected the guest, and the caller MUST
 * return immediately without touching r0-r3 or r15 - it is no longer being
 * asked to do its own job this time round.
 *
 * THE HOST MUST BE A PURE FUNCTION. Its call is deferred by one dispatch and
 * then repeated, so anything with a side effect would perform it twice. The
 * flag test at 0x02086204 is a bit test with no side effects, which is why it
 * is the one used. */
/* Ask the state machine for a SCREEN state by writing the request block it
 * writes to itself - eight words at manager + 1*0x20 + 0x48, the screen slot,
 * named (statereq.h, lsw2_state_request_in_slot, which records why it is not
 * the manager's current slot).
 *
 * This needs no guest call and no dispatch, which is why it works where both of
 * those did not: the guest-call seam refuses once a peer is connected, and a
 * trampoline only acts from inside a dispatch of its host, which stops
 * happening when the console leaves the lobby.
 *
 * Returns true once written. Call it from the port's tick or at a dataset
 * boundary - never from inside another request. */
bool lsw2_wh_write_state_request(uint32_t state_id);

bool lsw2_wh_trampoline_hop(uint32_t host_entry);

/* Is there a view manager right now? The one test, so callers cannot disagree
 * about what "present" means. */
/* THE INTENDED WAY INTO A MATCH: the host walks to the arena lobby's
 * console entity, its script opens the fork's console menu, and "Start Game"
 * brings up the CARTRIDGE'S wireless session and opens the CARTRIDGE'S Game
 * Settings - instead of the fork warping straight to the map.
 *
 * Returns true when it has taken ownership of the round start, so the caller
 * must not also warp. False means the real-WH route is off and the fork's own
 * path runs unchanged, which is what the baseline and the existing tests do.
 *
 * This replaces boot-time scaffolding: LSW2_WH_ROLE used to send one console
 * straight to state 54 and the other straight to state 34 before either player
 * had chosen anything, which is what made the two screens look unrelated. */
bool lsw2_wh_console_start();

/* Service a Start Game press that arrived before the session was up. The round
 * start is handed over exactly once, so without this the route stops at
 * "bringing the session up" and never finishes. Free when nothing is pending. */
/* The joiner's half of the same repair: open the CARTRIDGE'S wireless browse
 * list because the player chose "Join by IP/Room Code" at the arena lobby's
 * console, rather than because the fork aimed the console there at boot.
 * Returns true when it has taken the join; false when real-WH is off, leaving
 * the fork's own master-server join untouched. */
/* Put this console on the air when the host opens the lobby. The beacon has to
 * exist before a joiner's browse list has anything to list, and that is Start
 * Lobby's job rather than Start Game's. Inert when real-WH is off. */
void lsw2_wh_console_open_lobby();

/* SHUTTING THE LOBBY DOWN ENDS THE CARTRIDGE'S SESSION TOO. Ask for the
 * cartridge's own session end, and take the hop that runs it. See the long
 * note in whsession.cpp: it cannot be a call, because the teardown polls
 * WH_End in a do-while, so it rides the receive pump - the one hook measured
 * to fire on the guest's thread while a peer is connected. Requesting is what "Shut Down Lobby" does; the hop is taken from
 * the pump override in dispatch_overrides.cpp. */
void lsw2_wh_request_session_end(const char* why = "Shut Down Lobby");
bool lsw2_wh_session_end_hop(uint32_t host_entry);

/* The netgame tick, 0x020B07E8: one of the hop's two hosts, and the function
 * whose body reacts to a broken link (lsw2_wh_session_end_hop). */
constexpr uint32_t kLsw2NetGameTick = 0x020B07E8u;

/* THE JOINER'S HALF OF THE LOBBY SHUTDOWN - the console left behind when the
 * host shuts the lobby down raises a COMMUNICATION ERROR instead of leaving.
 *
 * Called once a frame from the netgame tick override with the cartridge's own
 * player count, because that is the one hook measured still firing after the
 * count drops to 1; the receive pump, which serves the host's half,
 * is guarded on `1 < count` and stops exactly when this is needed.
 *
 * Acts on the EDGE - a fall from two to one after a peer was really seated -
 * and only on a console whose ROLE is not host. See whsession.cpp for the full
 * causal chain, which was read out of the tick rather than guessed. */
void lsw2_wh_joiner_watch(unsigned players);

/* Close a session-end hop whose return is never coming. On the host the guest
 * comes back through the pump and the hop restores what it saved; on the joiner
 * the teardown nulls the view manager, the netgame tick stops being called, and
 * nothing ever claims the arm - measured. An arm left standing would restore a
 * stale lr and r0-r3 into an unrelated later call, so it is closed after two
 * seconds of frames without restoring anything. Called from the fork's
 * per-frame tick, which keeps running when the hook does not. */
void lsw2_wh_session_end_tick();

bool lsw2_wh_console_join();

/* Force lsw2_wh_console_join to open the cartridge's real DS-WM browse list (scan
 * -> associate), the same route LSW2_JOIN_WM=1 selects, without the env. The relay
 * internet-join (Lsw2ForkSession::join_relay) sets this: the peer is a
 * remote host reached over the relay, so the joiner MUST scan and associate the
 * cartridge's own way (the PC-transport "seat over TCP" route does not apply). */
void lsw2_wh_set_force_browse(bool on);

/* Per-frame tail for the net38 tolerance: call from netgame_tick_probe
 * AFTER the lifted tick so the consecutive-count-0 streak advances (or resets when
 * the count recovered). Paired with lsw2_fork_suppress_net38(), which the post-lift
 * guard calls at FUN_020b07e8's state-38 request. */
void lsw2_wh_net38_frame_end(void);

void lsw2_wh_console_start_tick();

/* KEEP THE CROSS-PLAYER CONSISTENCY CHECK DISARMED WHILE THE FORK'S LOBBY IS
 * UP - on whichever console is sitting in it.
 *
 * The fork's lobby is a loaded level, and loading it runs FUN_0208c888, which
 * arms the check WITHOUT re-seeding the shared random generator first. Every
 * other site that arms it seeds in the same breath, and every site that draws
 * from the generator disarms as it draws. So the fork leaves the check live
 * across a window where the two consoles' generators are legitimately apart,
 * and it fires and raises a Communication Error just as the match starts.
 *
 * Measured on both consoles by watching each one's OWN gate byte, and by the
 * word at 0x021092FC, which read 2 - "the cross-player check FAILED", a value
 * that is only reachable while this gate is set - on the console that had not
 * been disarmed.
 *
 * Only acts in the two PRE-MATCH screens (the arena lobby and Game Settings),
 * where no seed has been shared and the check has nothing meaningful to
 * compare. The real match state arms it itself, in the correct order, and that
 * arming is deliberately left alone. Called from the fork's per-frame tick. */
void lsw2_net_check_disarm_prematch();

bool lsw2_wh_have_view_manager();

/* HOST OR JOIN, from LSW2_WH_ROLE. Default host.
 *
 * A WH session needs one parent and one child. Measured: running
 * the same host path on two consoles produces two parents, both reaching
 * DATASHARING with `players 1` each - two solo sessions, not one match. So the
 * two instances take different doors, and both doors are the cartridge's own:
 * host runs 0x020B10A8 and goes to the chooser, join runs only the view-manager
 * allocator and goes to the browse list. */
bool lsw2_wh_role_is_host();

/* Set the wireless role from the in-game UI (Host vs Join), overriding the
 * LSW2_WH_ROLE env default. Without this two UI-launched windows both default to
 * host and never connect. Host on Start Lobby, Join on Connect. */
void lsw2_wh_set_role(bool is_host);

/* THE ARENA LOBBY'S MENU-STATE ID - where both consoles end up once the join is
 * accepted, and the state the host has to be sitting in before the
 * chooser is asked for again. Distinct from levels.h's kLsw2ArenaLobbyLevel,
 * which is the LEVEL number 9: one is which map is loaded, the other is which
 * menu state is running on top of it. */
constexpr int kLsw2ArenaLobbyState = 48;

/* THE TWO STATES A STARTED MATCH RUNS IN, and what they are needed for.
 *
 * 41 is the match state proper - the one `FUN_02072cf4`'s tag-3 branch requests
 * when the Start Game seed arrives, whose class re-seeds the shared generator
 * and re-arms the consistency check in the correct order. 42 is the pre-game it
 * advances to, which arms the round countdown.
 *
 * They are named here because the transition between them LOADS THE ARENA, and
 * a level load reads as "no level at all" for about a second and a half. Any
 * fork rule that treats an absent level as the player having quit must know
 * that the match is running, or it fires mid-transition - which is exactly what
 * `lsw2_lobbypin_is_quit` did, ending the session in roughly half of all runs.
 *
 * RETRACTED, the two descriptions above (the names are kept - they
 * are used): 41 is the statue select, statelog.h's kLsw2StateSelect, and 42 is
 * the match itself. Every measured match shows it: 41 lasts the select's ~30 s
 * countdown, and 42 lasts the round's three minutes and ends in the score
 * screen, 44. Read kLsw2MatchState as "the select" and kLsw2PreGameState as
 * "the match". */
constexpr int kLsw2MatchState = 41;
constexpr int kLsw2PreGameState = 42;

/* THE HOST'S JOIN-APPROVAL PROMPT - state 35 (0x23). When a peer connects the
 * cartridge puts the HOST here and its bottom screen reads "<name> is trying to
 * connect. Do you want to let them join your game?  Yes  No".
 * Answering YES (Left selects Yes, A confirms) is what actually completes the
 * connection - both consoles then converge on kLsw2ArenaLobbyState and the
 * cartridge counts two players for real. Left unanswered it is the whole of the
 * "peer connects but nobody sees anyone, signal red, countdown still advances"
 * symptom: `players 2` at the WM level is a connection with approval STILL
 * OUTSTANDING. */
constexpr int kLsw2JoinPromptState = 35;

/* The join prompt, auto-answered. Called once per frame from the
 * fork tick: when the host is sitting on kLsw2JoinPromptState it drives a
 * Left-then-A key sequence that answers YES, so the player never has to (and
 * never sees the vanilla prompt sit unanswered). lsw2_wh_join_answer_keys()
 * returns the synthetic key mask for the current frame and is OR-ed into the
 * guest's input; it has no side effects, so it is safe to read from the input
 * path. Fork + real-WH + host only; a no-op otherwise. */
void lsw2_wh_answer_join_prompt_tick();
uint16_t lsw2_wh_join_answer_keys();

/* THE JOINER'S "Waiting for Host" SCREEN - state 39, after the browse list
 * (coopjoin.h's kLsw2BrowseState, 34) has picked the host: the same title over
 * the names of the players already in the lobby, until the host's answer
 * brings every console to the reload, 40. Measured through a join:
 * joiner 34 -> 39 -> 40, host 35 -> 48 -> 40. */
constexpr int kLsw2JoinWaitState = 39;

/* THE STOCK JOIN SCREENS, NOT SHOWN. Once a frame from
 * the fork tick: while this console sits on one of the cartridge's wireless
 * screens a fork join passes through - 34, 35, 39, all answered without the
 * player - the touch screen leaves out their text layer, so the player sees
 * the loading screen's own backdrop until the loading screen itself (40).
 * Presentation only; see whsession.cpp. Fork only. */
void lsw2_wh_hide_stock_join_screens_tick();

/* Four players. The host stops accepting after the first
 * child (its beacon accept byte clears though maxEntry is 7), so a
 * 2nd/3rd/4th child refuses the non-accepting beacon and sits in SCANNING. While
 * the host is in WH DATASHARING this re-issues WM_SetEntry(1) periodically to keep
 * the parent accepting up to maxEntry. Gated behind LSW2_KEEP_ENTRY until proven.
 * See whsession.cpp. */
void lsw2_wh_keep_entry_tick();

/* THE JOINER'S HALF of the same idea, one screen earlier. Once the child's radio
 * is SCANNING and its "Browsing Wireless Games" list (state 34) is open, the scan
 * finds the host's beacon - but nothing selects it, so the cartridge's
 * own connect (0x020ADBCC) is never called and the child sits in SCANNING for
 * ever. This drives a synthetic A on a cycle to pick the highlighted host row,
 * after a short grace period that lets the scan populate the list first, so the
 * child connects without a player having to choose. It stops the instant the
 * radio leaves SCANNING (the connect has begun) or the list closes. Fork +
 * real-WH + joiner only; a no-op otherwise. lsw2_wh_browse_pick_keys() returns
 * the synthetic key mask for the current frame, OR-ed into the guest's input. */
void lsw2_wh_pick_browse_host_tick();
uint16_t lsw2_wh_browse_pick_keys();

/* Reopen the cartridge's Game Settings once a guest is really present.
 *
 * The probe that first asks for state 54 fires once, so after two consoles meet
 * in the lobby nothing could ask again - and the chooser is the screen that
 * starts the match. Host role only, because in the cartridge's own flow the
 * host picks the map and presses Start Game while a joiner waits. Gated on the
 * cartridge's own `players` count rather than a timer. */
/* THE OBJECT THE NEAREST-NODE SEARCH BELONGS TO - r0/r5 at the child's first
 * fault, from the log rather than from a symbol. Its table lives at +0x21C.
 *
 * NAMED SINCE: it is the LOCAL CAMERA, enhanced.h's kLsw2LocalCamera -
 * 0x020AAEB4 returns it, its yaw is +0x42 and its per-station movement mode
 * table is +0x208. One object under two names until the duplicate-constant
 * check put them side by side. */
constexpr uint32_t kLsw2NearestNodeOwner = kLsw2LocalCamera;

/* THE ACTOR ARRAY THE CAMERA TRIGGER SWEEP INDEXES.
 *
 * FUN_0200E4EC returns immediately unless its actor is
 * `*(0x02109358 + (u8)[netcount+0x2A] * 4)` - the ACTIVE player's actor. The
 * index byte lives in netcount.h's object and the trigger list in levels.h's
 * level root, so only this base needs a name. */
constexpr uint32_t kLsw2CameraActorArray = 0x02109358u;

/* Say once, on BOTH consoles, what the child has that the host does not.
 *
 * READ ONLY. The child faults on entering the shared lobby and the host does
 * not, so something differs; this prints the three candidates - the wireless
 * `owner` object's +0x06 and +0x0C (exactly what the join role skips by stopping after
 * the view-manager allocator) and the node table's pointer and count - so the
 * difference is measured before anything is written on the strength of it. */
/* Catch whatever builds the netgame node table, and print its address.
 *
 * The host's level reload builds the table at [0x02109C88 + 0x21C]; the child's
 * does not, and four candidate causes are already ruled out. Rather than guess a
 * fifth, this arms the branch ring once the level has come back with the table
 * still null and dumps it the moment the table appears.
 *
 * OFF unless LSW2_TRACE_NODETABLE=1: deep trace records both cores and stops
 * the run if left on, so the window is capped at kNodeTraceFrames either way. */
/* AN EXPERIMENT, off unless LSW2_WH_LOCAL_INDEX=1, and NOT a fix.
 *
 * Forces the child's active player index to 0 every frame, to test whether that
 * index is the whole difference between the two consoles. It is now the only
 * measured one: actor handling is byte-identical on both, each building and
 * clearing slot 1 three times during level loads and ending with it empty, so
 * the child's index points at a slot that is empty on BOTH.
 *
 * An earlier analysis rejected touching the index because the child's actor "belongs" at
 * slot 1; that half has since been contradicted, which is what reopens the
 * question. A result either way is information, and the write is removed once
 * it has answered - overwriting a value the cartridge computed is a probe, not
 * a repair. */
void lsw2_wh_local_index_experiment();

void lsw2_wh_nodetable_trace();

void lsw2_wh_join_gap_report();

void lsw2_wh_reopen_chooser_when_joined();

/* The joiner follows the host into the match. arm_* is called by the
 * fork tick when the host's start-match signal arrives; the tick then calls
 * lsw2_wh_joiner_enter_match() each frame, which fires once the guest is awake.
 * Behind LSW2_JOINER_ENTER_MATCH; no-op otherwise. */
void lsw2_wh_arm_joiner_enter_match();
void lsw2_wh_joiner_enter_match();

/* Bounded probe: force 41 -> 42 by requesting state 0x2a the way the
 * msg-0x0D handler does, once settled at 41. Behind LSW2_FORCE_MATCH. */
void lsw2_wh_force_match_state();

/* WHY THERE IS NO DIAGNOSTIC PASSTHROUGH HERE, and it is worth writing down.
 *
 * 0x020AD528 is the WH layer's printf, and routing it into the port's log
 * looked like a free win - the cartridge ships its own wireless diagnostics
 * and prints them somewhere nothing can see. An override was written and it
 * never fired ONCE.
 *
 * THE STRUCTURAL RULE is why: the recompiler emits a direct C call when a
 * branch target lands in the same generated bank, and only cross-bank or
 * indirect transfers reach runtime_dispatch where overrides live. 0x020AD528
 * is reached by `bl` from 0x020AF12C, ten instructions away, so the dispatch
 * table is never consulted. Every function in this module is in one bank, so
 * only the ones the game calls VIRTUALLY - through the vtable at 0x020FFD50 -
 * can be overridden at all. That is the constraint on the whole bring-up and
 * not a fact about this one function.
 *
 * The strings are still readable, just not that way: the stub that ran carries
 * the literal, so following it by hand gave WM_ERRCODE_ILLEGAL_STATE. */

/* WH_SYSSTATE_* as the guest currently has it, and its name. The name table is
 * the cartridge's own, at 0x020FFD20, so this cannot drift from what the game
 * means by the number. */
int lsw2_wh_state();
const char* lsw2_wh_state_name(int state);

/* Said once, and then only when the state changes. The whole point of the
 * bring-up is that this number moves. */
void lsw2_wh_report();

/* The manager's own address, so the readiness override and the report agree
 * about which object they are talking about. viewmask.h holds the context at
 * 0x0210B320; this is the POINTER to the C++ object whose vtable the session
 * start calls through, which is a different thing at a different address. */
constexpr uint32_t kLsw2WhManagerPtr = 0x0210B348u;

/* 0x020AD438, the readiness poll. Named here because dispatch_overrides.cpp
 * registers it and this file implements it. */
constexpr uint32_t kLsw2WhReadyPoll = 0x020AD438u;

/* THE DATA-SHARING STEP - vtable slot +0x18 of the wireless manager, at
 * 0x020FFD68. Named because dispatch_overrides.cpp registers it and this file
 * implements the reporting override. */
constexpr uint32_t kLsw2WhStepDataSharing = 0x020AD7F0u;

/* WH_End - the cartridge's own end-of-session, vtable slot +0x50 of the WH
 * manager at 0x020FFD50.
 *
 * FOUND THE WAY THE "No DataSet" WARNING WAS: the WH layer passes its own
 * function names to its printf at 0x020AD528, and the literal "WH_End" at
 * 0x02100094 has exactly one reference - from FUN_020ADD78. It takes no
 * arguments; it announces itself, sets the system state BUSY through
 * 0x020AD7A4, and calls the WM layer's end at 0x020D71C8.
 *
 * NOT CALLED YET, AND THE REASON IS RECORDED RATHER THAN THE ATTEMPT KEPT.
 * Shutting the lobby down needs this: shut_down_lobby() closes the fork's TCP
 * session and leaves the cartridge's wireless session up. Wiring it through
 * the session trampoline was tried and the hop was ARMED BUT NEVER TAKEN -
 * flag_test_array_c is the Free Play widget constructor's flag test, a
 * construction-time call rather than a per-frame one, so it stops being
 * dispatched once boot is over. That host had already been measured
 * dispatching five times in a whole run; this assumed it would be dispatched
 * "here" without checking, which is the same mistake again.
 *
 * So the address is kept, the mechanism is not. What the shutdown needs is a way to
 * call a cartridge function while a session is live - the same open problem the
 * state request solved by writing the machine's request block directly, which
 * has no equivalent for a function call. */
constexpr uint32_t kLsw2WhEnd = 0x020ADD78u;

/* Bring the cartridge's wireless session down by calling its own WH_End.
 *
 * Used by Shut Down Lobby. Does nothing when no session is up. It is a plain call
 * rather than a guest_call because WH_End is lifted and takes no arguments;
 * see whsession.cpp for why the trampoline hunt was unnecessary. */
void lsw2_wh_end_session();

/* 0x0207281C, the view-manager allocator. Called BY SYMBOL through the guest
 * seam rather than overridden, so this is the address the seam reports the
 * call against - see lsw2_wh_make_view_manager. viewmanager.h has walked its
 * tail before; this is the first time the port runs it. */
constexpr uint32_t kLsw2ViewManagerAlloc = 0x0207281Cu;



/* THE FIRST METHOD THE MACHINE CALLS ON A NEW STATE - vtable slot +0x08, which
 * for state 54 is 0x0208E374.
 *
 * FUN_0205F45C is the pump's per-slot construction step and it is explicit:
 *
 *     uVar1 = FUN_0205f574(machine, record[0x48], ... );   the factory
 *     *(machine + slot*4) = uVar1;                          publish it
 *     (**(code **)(*state + 8))();                          <- here
 *
 * so this runs immediately after the constructor, before anything else touches
 * the object. That makes it the earliest interceptable moment in the state's
 * life - the constructor itself is a direct `bl` from the factory's arm and
 * cannot be overridden.
 *
 * AND SLOT 3 IS NOT THE UPDATE. 0x0208E514 was identified as "slot 3 of state
 * 54's vtable", which is true, and an override there never fired once. The
 * machine makes exactly four virtual calls on a state - +0x04, +0x08, +0x10
 * and +0x14 - and +0x0C is not among them. Slot 3 is reached from inside one
 * of those when the menu is acted on. */
constexpr uint32_t kLsw2ChooserEnter = 0x0208E374u;

/* STATE 54'S NUMBERS, IN PROSE RATHER THAN CONSTANTS, because nothing in the
 * port uses them yet and a constant with no reader is a claim nobody checks.
 * They are written down because they were expensive to find:
 *
 *   the state id is 0x36; its object's SELECTION is at +0xEC and its default
 *   TARGET at +0xF0, which the constructor sets to 0x39 - state 57, NOT
 *   uninitialised as an earlier note here claimed;
 *
 *   FUN_0208EA58 switches on the selection and 1 is the wireless case, the
 *   branch that calls 0x020B10A8;
 *
 *   the UPDATE is 0x0208E514, slot 3 of the vtable at 0x020FF524, and its only
 *   reference in the image is that slot - so it is virtual and interceptable;
 *
 *   the CONSTRUCTOR is 0x0208E260 and is NOT interceptable: its one reference
 *   is `bl 0x0208e260` from the factory's arm, a direct call in the same bank.
 *
 * dup_constants flagged +0xEC against dispatch_overrides.cpp's pause cursor -
 * two different structures that share an offset - which is exactly the kind of
 * coincidence a named constant would have made look like a relationship. */

#endif /* LSW2_WHSESSION_H */
