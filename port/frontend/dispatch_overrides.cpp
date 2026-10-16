/* The port's dispatch overrides: our C in place of the guest's.
 *
 * WHY THIS FILE EXISTS AND WHERE IT IS CALLED FROM. `nds_register_dispatch`
 * appends, and lookup answers with the FIRST bank that matches - so a table
 * registered after the title banks is never reached. Our shell does not start
 * until main() has finished registering them, so there is no moment in our own
 * code that is early enough. Patch 0006 adds a hook in main.cpp that calls this
 * immediately before the title banks go in.
 *
 * THE ACCURACY FLOOR IS UNTOUCHED. The lifted body of every function here is
 * still linked and still correct; this table decides which one runs. That is
 * the framework's own rule - "the recompiled/LLE implementation is the accuracy
 * floor and remains linked, runnable, and forceable" - and it is why an
 * override that turns out to be wrong is a switch away from being disabled
 * rather than a rebuild away.
 *
 * SO THIS FILE IS OPT-IN. LSW2_HLE=1 turns it on. Off by default because
 * nothing here is verified against the lifted bodies yet, and an unverified
 * replacement running by default would be exactly the "plausible and wrong"
 * failure this project keeps recording.
 */

#include "characterpick.h"
#include "viewmask.h"
#include "cheats.h"
#include "customization.h"
#include "commguard.h"
#include "forksession.h"
#include "fork.h"
#include "lasthunter.h"
#include "coopjoin.h"
#include "forkpopup.h"
#include "popuphud.h"
#include "sessionroster.h"
#include "freeplaypick.h"
#include "hatstay.h"         /* a hit keeps the hat - the knock-off's spawn */
#include "carbonitekeep.h"   /* a capture keeps the customization */
#include "headfix.h"
#include "gearhop.h"
#include "probehooks.h"
#include "hudportrait.h"     /* kLsw2HudStep - the HUD portrait hop's host */
#include "partyportrait.h"
#include "whsession.h"
#include "levelhud.h"   /* kLsw2LevelStateMessage */
#include "statelog.h"        /* lsw2_state_current - the 41 -> 42 force gate */
#include "netmsglog.h"       /* the netgame message ring layout - message relay */
#include "hostsettings.h"
#include "behaviourhook.h"
#include "markerhook.h"
#include "statereq.h"
#include "pauselayout.h"
#include "pauseredraw.h"
#include "scriptqueue.h"
#include "selectwidget.h"
#include "strings.h"
#include "ui.h"

#include "../net/lsw2_lockstep.h"
#include "../net/lsw2_net_bind.h"
#include "vwh.h"
#include "sessionpause.h"
#include "netcount.h"         /* kLsw2NetCountObject - the party spawn's player count */

#include "runtime_arm.h"
#include "state.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

extern "C" void lsw2_arm9_FUN_0208cfec(void);
extern "C" void lsw2_arm9_FUN_02060158(void);
extern "C" void lsw2_arm9_FUN_020b0410(void);
extern "C" void lsw2_arm9_FUN_020b1248(void);   /* the netgame message dispatcher */
extern "C" void lsw2_arm9_FUN_0208e514(void);   /* the pause state's update */
extern "C" void lsw2_arm9_FUN_020b8e40(void);   /* between the manager's two passes */
extern "C" void lsw2_arm9_FUN_020b07e8(void);
extern "C" void lsw2_arm9_FUN_02088014(void);
extern "C" void lsw2_arm9_FUN_0204a620(void);
extern "C" void lsw2_arm9_FUN_02055530(void);
extern "C" void lsw2_arm9_FUN_02018010(void);
extern "C" void lsw2_arm9_FUN_0202c218(void);
extern "C" void lsw2_arm9_FUN_0204a620(void);
extern "C" void lsw2_arm9_FUN_0208cdb0(void);
extern "C" void lsw2_arm9_FUN_02055530(void);
extern "C" void lsw2_arm9_FUN_02097bcc(void);
extern "C" void lsw2_arm9_FUN_02050c44(void);

namespace {

/* WH_GetSystemState, the cheapest possible proof that the seam works.
 *
 * The guest function is nine instructions:
 *
 *     0x020AD310  ldr r0, ->0x0210B320     the WH context
 *                 ldr r0, [r0, #0x20]      the state
 *                 bx  lr
 *
 * Chosen first for exactly that reason - it has seven callers, no side effects,
 * no netcode, and a return value that is trivially comparable against the
 * lifted body. If the seam does not work for this, it does not work.
 *
 * IT DOES THE SAME THING AS THE GUEST, deliberately, at this stage. The point
 * of the first override is to prove the mechanism, not to change behaviour: if
 * this returned something invented, a difference in the game could not be
 * attributed to the seam rather than to the invention. Once the wireless HLE
 * owns the context, this is where its state is reported from - and the values
 * are pinned by the name table at 0x020FFD20, so it will not have to
 * guess them.
 */
/* See viewmask.h - the wireless context and its fields live there. */

uint64_t g_hits = 0;

void wh_get_system_state() {
    g_cpu.R[0] = bus_device_read32(9, kLsw2SessionAuthority + kLsw2WhSystemState);
    if (g_hits == 0) {
        /* ONCE, THE FIRST TIME IT ACTUALLY RUNS. Registering a table and
         * reaching it are different claims, and this project has already been
         * caught out three times by a call that was accepted and inert. */
        std::fprintf(stderr, "[hle] WH_GetSystemState override REACHED "
                             "(returned %u)\n", unsigned(g_cpu.R[0]));
    }
    ++g_hits;
    /* Return the way the guest does. A lifted function ends by branching
     * through LR; ours has to leave the machine in the same state or the
     * caller resumes somewhere else entirely. */
    g_cpu.R[15] = g_cpu.R[14];
}

/* A REACHABILITY PROOF, because the one above cannot be one.
 *
 * WH_GetSystemState only runs during wireless activity, so registering it
 * proves the table was accepted and nothing more - and "accepted" is exactly
 * the claim this project has been burned by three times. This second override
 * is a function that definitely runs in the Arena Lobby: the flag test at
 * 0x02086204, which the Free Play widget's constructor calls to decide party
 * mode versus roster mode.
 *
 * It is PURE - a bit test with no side effects - and the whole body is known:
 *
 *     asr  r2, r1, #5
 *     add  r0, r0, r2, lsl #2
 *     add  r0, r0, #0x4000
 *     ldr  r0, [r0, #0x488]
 *     and  r1, r1, #0x1f
 *     tst  r0, #1 << r1
 *     r0 = tested ? 1 : 0
 *
 * so it can be reproduced exactly rather than approximated. That matters: an
 * override that is subtly different would change the widget's mode and the
 * failure would look like a game bug rather than a seam experiment.
 */
/* THE ARENA-LOBBY FLAG TEST'S OWN ADDRESS, needed as a RETURN address by the
 * view-manager hop below.
 *
 * THE TABLE ENTRY DELIBERATELY KEEPS ITS LITERAL. Replacing it with this name
 * was the tidier-looking change and it broke five sweeps at once -
 * override_targets, build_gating, log_reachable, call_reachable and
 * sweep_lr_checks all read this table by parsing the address out of each row,
 * and sweep_lr_checks then reported that kFreePlayCtorReturn 'calls
 * 0x02086204, which is NOT overridden'. The tools are the project's check that
 * an override is really reachable, so the table stays in the form they can
 * read. The duplicated value is the price and it is cheap: one of the two is a
 * registration and the other is an lr, which are different things that happen
 * to share a number. */
constexpr uint32_t kFlagTestArrayC = 0x02086204u;

/* THE CARTRIDGE'S RECEIVE PUMP, and the reason this file needs it.
 *
 * netcount.h: it is "called every frame from the state manager's tick". It was
 * established that NO fork override fires on the guest's thread once the lobby
 * settles - the flag-test hook is construction-time only - and the graceful
 * "Shut Down Lobby" return and the lobby respawn both need exactly that,
 * because the cartridge's session teardown was shown to contain a blocking poll
 * loop and so can never be called from the host, only run on the guest's own
 * thread.
 *
 * ITS REACHABILITY WAS CHECKED BEFORE THIS WAS WRITTEN, which is the check
 * that three pieces of dead work were the price of learning: it is lifted, nothing
 * else overrides it, it has exactly ONE caller (FUN_0205ed08 at 0x0205ED08),
 * and that caller is in generated bank 17 while the pump is in bank 32 - so the
 * transfer is cross-bank and goes through runtime_dispatch, where overrides
 * live. */
constexpr uint32_t kNetMsgPump = 0x020B0410u;

/* THE PER-FRAME NETGAME TICK, and why the joiner's half of the "Shut Down
 * Lobby" return needs THIS hook and not the pump.
 *
 * It was read end to end. It is reached from the main loop
 * (0x02000BC0 -> 0x020013A4 -> here), and it is the function that requests
 * STATE 38 - the Communication Error - when the player count has changed and the
 * view manager's counter returns zero.
 *
 * ITS GUARD IS THE POINT. The pump is only called while
 * `1 < [0x021027F8 + 0x29]`, and that byte is the popcount that has just dropped
 * to 1 when a peer leaves - so a hop armed then is never taken. This
 * function's only gate is `if (*0x021092F4 == 0) return;`, a null check on the
 * view manager, which is still non-null at exactly the moment the joiner needs
 * to act.
 *
 * REACHABILITY CHECKED BEFORE THIS WAS WRITTEN, the same four questions
 * that three pieces of dead work were the price of learning: lifted, not already
 * overridden, not called by symbol from any other bank, and cross-bank - the
 * caller FUN_020013a4 is in lsw2_arm9_00.c and this is in lsw2_arm9_32.c, so the
 * transfer goes through runtime_dispatch. */
/* kLsw2NetGameTick (whsession.h), 0x020B07E8. */

/* THE TWO STATE-UPDATE SLOTS THAT LOOK LIKE PER-FRAME HOOKS AND ARE NOT.
 *
 * ELIMINATED BY MEASUREMENT. Re-applying customization after Free Play
 * character switches, and rendering the Jango/Boba head under customize
 * headwear, both need a per-frame guest-thread hook to hop the gear applier
 * from, and the pump calls the
 * CURRENT state's vtable +0x10 every frame - virtually, which ought to mean it
 * reaches the dispatch table. Read off the live machine, those slots are
 * 0x0208CF08 (the gameplay state, vtable 0x020FF478) and 0x020877A0 (state 48,
 * Free Play, vtable 0x020FF414).
 *
 * BOTH WERE REGISTERED AND NEITHER FIRED ONCE - not a slow rate, no call at
 * all, while the same instrument in the same run reported a first call for
 * script_command, state_message and flag_test_array_c. The addresses are not in
 * doubt: the same vtable read gives +0x14 = 0x0208CFEC, which is state_message,
 * already registered and firing.
 *
 * So slot +0x14 of that vtable dispatches and slot +0x10 does not, and no
 * override on either update can be built on. Recorded here so the next reader
 * recognises them rather than re-deriving and re-registering them. */

/* STATE 48'S +0x14 - THE SLOT THAT IS KNOWN TO DISPATCH.
 *
 * The two +0x10 update slots were measured and neither fired ONCE, while
 * `state_message` on 0x0208CFEC - the GAMEPLAY state's +0x14 - fires perfectly
 * well. So the difference is the SLOT, not the state, and the obvious candidate
 * was never tried: state 48's OWN +0x14, read off the same live vtable in the
 * same breath as its +0x10 and then left alone.
 *
 * State 48 is current while the player is in Free Play picking characters,
 * which is exactly when the gear-applier hop needs a hook. */
constexpr uint32_t kFreePlayMessage = 0x02088014u;

/* FOUR PER-FRAME HOOK CANDIDATES, and this time they were MEASURED not read.
 *
 * port/patches/0010 makes runtime_dispatch histogram the PCs it is
 * actually consulted for - which is exactly the set an override can hook, and
 * something the branch ring cannot distinguish. Three settled-lobby frames gave
 * 4351 distinct dispatched PCs over 81038 dispatches; filtering to the game's
 * own code, to counts of 3-6 (about once per frame), and then intersecting with
 * the recompiler's LIFTED FUNCTION ENTRIES leaves a validated shortlist.
 *
 * Seven earlier candidates were picked by reading code and all seven were
 * refuted. These four come off that list instead, and they are registered
 * together so ONE run ranks them rather than one build per guess. */
constexpr uint32_t kFrameCand0 = 0x02018010u;
constexpr uint32_t kFrameCand1 = 0x0202C218u;
constexpr uint32_t kFrameCand2 = 0x0204A620u;
constexpr uint32_t kFrameCand3 = 0x02055530u;




uint64_t g_flag_hits = 0;
uint64_t g_bounds_hits = 0;
bool g_force_free_play = true;

/* The instruction after the Free Play widget constructor's flag test - which is
 * what lr holds when THAT caller reaches this override, and nothing else does.
 * The customize screen asks the same question from 0x0207AA30. */
constexpr uint32_t kFreePlayCtorReturn = 0x02068384u;

/* WHERE THE TAP HANDLER ASKS WHETHER A CHARACTER IS UNLOCKED.
 *
 * 0x020687B4 holds the select widget in r6 throughout, and its mode-0 path -
 * the Free Play select's path - calls the save-flag query twice. These are the
 * two return addresses, and they are how the fork learns which widget the
 * player is touching. See the note at the capture. */
/* Where the tap handler hands the fork the widget the player is touching -
 * two RAM scans failed to find it and the game volunteers it here. Used by
 * an override of 0x02086534, which is not currently installed - see the note
 * beside flag_test_array_c for why it was removed. */
constexpr uint32_t kSelectTapUnlockQueryA [[maybe_unused]] = 0x02068880u;
constexpr uint32_t kSelectTapUnlockQueryB [[maybe_unused]] = 0x020688B8u;

/* The hunter-only page range MOVED TO selectwidget.h as
 * kLsw2SelectHunterPageLow/High, with its derivation from the page titles, when
 * fork.cpp became a
 * second file that has to apply a range to this widget. */

/* levels.h id 9, "Cantina Arena Lobby", source "confirmed". The same level
 * fork.cpp warps to; kept as its own constant here rather than reaching into
 * that file's anonymous namespace. */
/* The lobby destination is levels.h's kLsw2ArenaLobbyLevel, EMITTED from the
 * generated table by name. This file used to declare it again. */

/* ------------------------------------------------------------------------- */
/* The lobby console's save gate, waived - and online play given the entry
 * point it never had.
 *
 * HOW THIS WAS FOUND, because it took four passes and only the last one could
 * have worked. "You must Save before doing that!" is string 0x2D7, and that
 * value appears NOWHERE in the ARM9 image at any alignment; no get_string call
 * site can compute an id above 0x2AA. The reason is that the id is not in the
 * program at all - it is an argument in a LEVEL SCRIPT.
 *
 * A runtime trace gave the chain, and the frame that mattered was
 * invisible to the first backtrace because it is reached through `BLX`:
 *
 *     0x0205FFB8   the script VM's command loop
 *       -> 0x0206004C  BLX vtable[5]
 *     0x0208CFEC   the command dispatcher, opcodes 0..0x1A
 *       -> case 26   0x0208D12C
 *     0x0208B3F0   show a message popup, id = args[0]
 *
 * Commands are packed words in a per-player queue: the LOW halfword is the
 * opcode, the HIGH halfword is the argument count, and the arguments follow
 * inline. `0x02060084` is the one function that writes them, and it is pure
 * memory - no guest calls - so the seam can replace it faithfully.
 *
 * WHAT THE FORK DOES WITH IT. Suppressing the popup alone would remove the
 * message and not the refusal, which is not what "waived" means. But the
 * fork's design already says what interacting with that console should do -
 * it opens the fork's own menus - so the gate becomes the trigger:
 *
 *   offline  -> the host settings menu, which is "Host Internet Game"'s
 *               destination and ends in "Start Lobby"
 *   hosting  -> the console entity's six options
 *
 * That is online play's first reachable entry point. Before it, every piece of
 * online play was built and none of it could be opened from the game; this is
 * the door.
 *
 * THAT PARAGRAPH ABOVE IS THE CURRENT BEHAVIOUR ONLY FOR THE HOSTING CASE.
 * Offline now opens the THREE-OPTION ARENA MENU, not the host settings menu -
 * fork.cpp's console handler says why: opening host settings directly skipped
 * the step where the player chooses to host rather than to join, "and joining
 * is half of what that screen is for".
 *
 * THIS COMMENT USED TO SAY THAT MENU WAS STILL MISSING, in these words: "the
 * three-option arena menu has no screen of its own, so joining someone else's
 * lobby is still unreachable ... which state class draws that menu is
 * the five-times-unanswered question."
 *
 * It is not missing and joining is not unreachable. The chain is:
 *
 *     the console entity, offline   -> Lsw2ForkSession::open_arena_menu()
 *     screen_ = ArenaMenu           -> lsw2_arena_menu_build(menu_)
 *     kArenaJoin                    -> open_join_by_code()
 *     kArenaMaster                  -> open_master_list()
 *
 * And that question was not answered, it was RETIRED: it asked which
 * cartridge state class draws that menu, and the design settled that the port
 * draws its own menus in the game's style. There is no state class to find.
 *
 * The claim is left visible rather than deleted because a comment saying half
 * of online play is unreachable is the kind a reader believes and acts on - and this
 * one sat four lines above the code that had already made it false.
 *
 * SCOPE. Only this one message id, and only in the fork. Opcode 26 carries
 * every in-level hint in the game - the whole 0x2B7-0x2D1 block - and those
 * must keep working. */
/* The state message handler, and the opcode that raises a popup. Both read off
 * 0x0208CFEC's own jump table: `CMP r1, #0x1A` bounds it and the entry for 26
 * lands on 0x0208D12C, which does `LDR r1, [r0]` then calls 0x0208B3F0. */
constexpr uint32_t kScriptEnqueueWrapper = 0x02060158u;
/* The handler is kLsw2LevelStateMessage (levelhud.h), 0x0208CFEC. */
constexpr uint32_t kStateMsgPopup = 26u;

/* THE MESSAGE THAT SETS A PORTRAIT'S CHARACTER - case 0x15 of this same
 * handler, which runs
 *
 *     FUN_0208c024(state + 0x2c, args[0], args[1], args[2]);
 *
 * and that in turn tears the portrait down (FUN_0208b54c) and rebuilds it for
 * character args[0] (FUN_0208b76c), picking the face texture and palette from a
 * table indexed by the character with stride 0x18. That path was traced from
 * the pixels, then proved by rewriting the chosen texture on the running game
 * and watching the icon change. */
constexpr uint32_t kStateMsgSetPortrait = 0x15u;

constexpr uint32_t kMsgMustSave = 0x2D7u;

/* The console's OTHER refusal, and the one that proved flag 11's meaning.
 * config/strings.json: "You can't do that during a Wireless game!". */
constexpr uint32_t kMsgWirelessBusy = 0x2D5u;

/* WHERE THE CONSOLE MAY OPEN, for all three hooks that decline its refusal.
 *
 * The script-command hook was gated to the lobby STATE (48), because the
 * statue select (41) and the rules chooser (54) run on the same level and the
 * console's script still answers the host's A there. The other two hooks - the
 * queue and the state message - kept the level test only, so the refusal the
 * command hook now let through reached the queue and opened the console
 * anyway: the host pressing confirm during character select or the match
 * could open the lobby console. Measured
 * (conn_confirm1): three "console interaction: screen state 41" from three A
 * presses in the select, every one through the queue.
 *
 * And not while ALL's pre-game pick is up: it runs in state 48 on the Free Play
 * grid, where A is the lock-in. */
bool console_open_allowed() {
    return lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel &&
           lsw2_state_current() == kLsw2ArenaLobbyState &&
           !lsw2_freeplaypick_pregame_active();
}

/* The Cantina Arena Lobby. Same id fork.cpp warps to - kept as its own constant
 * here rather than shared, because this file must not depend on fork.cpp's
 * internals and one number in two places is cheaper than the coupling. */
/* ...and so did this, 84 lines below, under a second name. */

uint64_t g_enqueue_hits = 0;

/* SAY ONCE THAT THIS SEAM IS ALIVE - and the answer is now known.
 *
 * This used to read "the save-gate waiver lives in the enqueue override and has
 * never fired, in either of its two forms", and offered two explanations: the
 * command never reaches the function, or it reaches it and the test is wrong.
 *
 * **Neither was the answer.** The override was registered on 0x02060084 and
 * 0x020600F0, both of which share a generated bank with their only callers, so
 * the recompiler emitted direct symbol calls and the dispatch table - where an
 * override lives - was never consulted. The seam was not being entered because
 * it could not be. `tools/sweep_override_reachable.py` measures that now, and
 * `tools/sweep_lr_checks.py` refuses an `lr` gate sitting under such a call.
 *
 * The waiver lives on 0x02060158 instead (see script_enqueue_wrapper), which has
 * no direct call sites at all. Kept as a counter rather than a bool because
 * "how many" distinguishes a seam that ran once from one that runs constantly,
 * and the trace below is priced on the second. */

/* The guest bus, behind scriptqueue.h's abstract memory. The test supplies a
 * map instead, which is the whole reason the body is not written inline here.
 * Still used by the fork's OWN popup route, which is why these outlived the
 * override that introduced them. */
uint32_t script_read32(uint32_t addr, void*) { return bus_device_read32(9, addr); }
void script_write32(uint32_t addr, uint32_t value, void*) {
    bus_device_write32(9, addr, value);
}

/* THE OLD ENQUEUE OVERRIDE LIVED HERE, and it is deliberately gone rather
 * than kept for reference.
 *
 * It was registered on 0x02060084 and 0x020600F0 and could never run: the
 * recompiler emits a direct C call for a branch whose target is in the same
 * generated bank, and both addresses share a bank with their only callers.
 * Its live parts - the script-target note, the LSW2_TRACE_ENQUEUE trace and
 * the save-gate waiver - now sit in `script_enqueue_wrapper` on 0x02060158,
 * which has no direct call sites at all.
 *
 * Keeping a dead override next to a live one is the exact trap this project
 * just spent four passes in: it reads as working code, it has a test, and it
 * cannot execute. `tools/sweep_override_reachable.py` now catches the class;
 * leaving an unregistered copy behind would sit outside what that gate can
 * see. */


/* ------------------------------------------------------------------------- */
/* THE PAUSE MENU'S HIDDEN ROWS (Host Wireless, Extras, Save Game), done
 * properly this time.
 *
 * Blanking the three labels was the first plan and it half-worked. Play found
 * what that plan itself predicted: the rows vanish, but the cursor
 * still stops on them, and row 1 keeps drawing a DS icon that is not part of
 * its label. Both halves need the menu's own structures, and those are now
 * read rather than guessed.
 *
 * THE BUILDER IS 0x0208E874, and it is the array that four refuted attempts
 * tried to own. It copies six string ids out of a table at
 * 0x020F4D00 onto its stack, then draws them at y = 0x28 + row*0x19 - which is
 * the measured first row 40, pitch 25, exactly.
 *
 *     row 0  0x0040  Resume
 *     row 1  0x003C  Host Wireless      (0x3D / 0x3E while wireless is up)
 *     row 2  0x004D  Settings
 *     row 3  0x0007  Extras             <- settles an earlier three-way guess
 *     row 4  0x0101  -> 0x002C Save Game in-game
 *     row 5  0x0044  Quit Game
 *
 * THE ROWS CANNOT BE COMPACTED, and this was worth checking before trying.
 * The activation handler at 0x0208EA58 switches on the cursor INDEX:
 *
 *     0208EA64  LDR r0, [r9, #0xEC]     the cursor
 *     0208EA6C  ADDLS pc, pc, r0, LSL #2
 *
 * so index 2 *is* Settings, not "whatever the third visible row says". Removing
 * entries from the id list would leave every action pointing at the wrong
 * screen - a much worse bug than the one being fixed, and an invisible one
 * until someone pressed A.
 *
 * So six rows stay six rows, and the fork hides three of them by making the
 * cursor refuse to rest there. */
/* THE PAUSE ROW LOOP, IDENTIFIED TWO FRAMES UP.
 *
 * Seen twice in play: the wireless icon still drawn and the cursor still
 * resting on hidden rows. Both because `pause_menu_fixups()` HAS NEVER RUN.
 *
 * It was gated on `R[14] == 0x0208E9CC`, the return address in the pause menu's
 * row loop. That address is real - the loop does `BL 0x02066708` at 0x0208E9C8 -
 * but the override is not registered on `0x02066708`. It is registered on
 * `0x0205CD20`, and there is a frame in between:
 *
 *     0208E9C8  BL 0x02066708          the pause loop        lr = 0x0208E9CC
 *       02066708  STM sp!, {r3, lr}    the wrapper pushes lr
 *       02066714  BL 0x0205CD20        <- WE ARE HERE        lr = 0x02066718
 *
 * So the condition could never be true, and every previous "fix" to the pause
 * menu was written on top of a function that does not execute. That is why two
 * builds showed the identical symptoms.
 *
 * THE WRAPPER SAVES WHAT WE NEED. `STM sp!, {r3, lr}` is a push of exactly two
 * words, so at our entry the pause loop's return address is at `sp + 4` - and
 * the wrapper touches no other registers, which is also why r6, r7 and r10 are
 * still the loop's when we get here. */
constexpr uint32_t kStringWrapperReturn = 0x02066718u;  /* lr inside 0x02066708 */
constexpr uint32_t kPauseRowReturn = 0x0208E9CCu;       /* lr in the row loop  */
constexpr uint32_t kWrapperSavedLr = 4u;                /* [sp+4] after its push */
constexpr uint32_t kPauseRowCount = 6u;
constexpr uint32_t kPauseCursorField = 0xECu;
constexpr uint32_t kPauseIconField = 0x30u;

/* The pause menu's hidden rows and the layout arithmetic now live in pauselayout.h,
 * where they are tested - this menu took three attempts and the third could
 * not execute at all, so the one part that needs no guest is pinned. The
 * table below is still needed for the touch-kind report, which walks the
 * same three rows. */
/* See pauselayout.h - kLsw2PauseHiddenRows is the one copy. */

uint32_t g_pause_last_cursor = 0;
bool g_pause_reported = false;

/* THE MENU OBJECT, REMEMBERED, AND THE STATE IT BELONGED TO.
 *
 * Two symptoms: the pause menu still showed the DS wireless icon where Host
 * Wireless had been, and scrolling did not jump to the next visible row
 * properly.
 *
 * Both are one cause. The fixups ran ONLY from inside the string-decode
 * override, which the menu enters while BUILDING ITS ROWS - so they fired once,
 * before the player had touched anything. The cursor nudge cannot work that way
 * by construction: the player scrolls AFTER the build, and nothing was watching
 * by then. The icon has the same shape - anything that re-sets the flag after
 * the build is uncontested.
 *
 * So the object is remembered here and the same corrections are re-applied
 * every frame while the menu is up. The state kind is recorded ALONGSIDE it
 * rather than hard-coded: writing into a stale menu object after the state has
 * gone is how a hiding fix becomes a corruption bug, and comparing against the
 * kind observed at build time needs no magic number and cannot be wrong about
 * which state this was. */
uint32_t g_pause_obj = 0;
int g_pause_kind[2] = {-1, -1};

/* THE PAUSE MENU IS TOUCH-DRIVEN, AND THE CURSOR NUDGE DOES NOT COVER THAT.
 *
 * The three rows must be inoperable and hidden. Hidden is the blank
 * label; inoperable was the cursor refusing to rest on them - which is the whole
 * story only if the d-pad is the whole input. It is not. `0x0208E640` reads the
 * stylus point out of `0x021044A4 + 0x2A/+0x2C` and hit-tests it against a
 * per-row record at **`menu + 0x98 + row*0x0C`**:
 *
 *     +0x00  the rectangle, passed to 0x02064A58(rect, point)
 *     +0x01  an ENABLED byte
 *     +0x02  a halfword compared against 0x0A
 *
 * and then, on a hit:
 *
 *     cursor == row && [+0x01]     -> activate  (0x0208EA58)
 *     [+0x02] >= 0x0A              -> activate, WITHOUT consulting the cursor
 *     otherwise, if [+0x01]        -> move the cursor here and rebuild
 *
 * So a tap could reach a row the cursor is not allowed to sit on. The label is
 * blank, so the player would not see what they hit - which makes it worse, not
 * better: a stray tap in a blank strip firing "Host Wireless" is exactly the
 * kind of bug that gets reported as "the game randomly went to wireless".
 *
 * CLEARING `+0x01` IS THE CARTRIDGE'S OWN LEVER and it closes the first and
 * third arms. The second arm is NOT closed, because what `+0x02` means has not
 * been read and writing a guess into it is how this project has gone wrong
 * before - so it is REPORTED instead, once, and a run says whether it is
 * live for these rows. */
constexpr uint32_t kPauseRowRecords = 0x98u;   /* menu + this + row*0x0C */
constexpr uint32_t kPauseRowStride = 0x0Cu;
constexpr uint32_t kPauseRowEnabled = 0x01u;
constexpr uint32_t kPauseRowTouchKind = 0x02u;

/* THE ROW LAYOUT, read straight out of the builder's loop at 0x0208E980:
 *
 *     0208E984  MOV  r7, #0x28              Y starts at 40
 *     0208E9E0  MOV  r2, #0x80              X is 128 - already centred
 *     0208E9E4  MOV  r3, r7                 ...and r7 is the Y it draws at
 *     0208E9E8  BL   0x02065BE8             draw the row's text
 *     0208EA24  ADD  r7, r7, #0x19          +25 PER ROW, hidden or not
 *     0208EA2C  ADD  r6, r6, #1
 *     0208EA30  LDR  r0, [r10, #0xE8]       the row count
 *
 * The last two lines are the problem. The rows that remain visible must be
 * centred on the pause screen and placed one after another, so the menu does
 * not look as if options are missing.
 *
 * Y advances unconditionally, so blanking three of six rows leaves three
 * 25-pixel holes and a menu that reads as broken rather than as short.
 *
 * THE FIX NEEDS NO NEW SEAM. The string decode this file already overrides is
 * called from INSIDE that loop - `lr` is 0x0208E9CC - with r6 holding the row
 * index and r7 holding the Y about to be drawn. Rewriting r7 on the way out
 * repositions the row, because the override returns to a caller that has not
 * re-read it. */
/* The first row's Y and the per-row pitch are pauselayout.h's
 * kLsw2PauseFirstY and kLsw2PausePitch. This file used to declare both again
 * as kLsw2PauseFirstY and kLsw2PausePitch - and pauselayout.h exists BECAUSE
 * the layout arithmetic was the part most likely to be broken by a later
 * edit, which a second copy in this file defeats entirely. */

uint8_t read_guest_byte(uint32_t addr) {
    const uint32_t w = bus_device_read32(9, addr & ~3u);
    return uint8_t((w >> ((addr & 3u) * 8u)) & 0xFFu);
}

void write_guest_byte(uint32_t addr, uint8_t v) {
    /* READ-MODIFY-WRITE, because the bus helper is word-wide and this byte has
     * three neighbours that belong to the same record. Writing the word with
     * only this byte set would clear the rectangle beside it. */
    const uint32_t base = addr & ~3u;
    const uint32_t shift = (addr & 3u) * 8u;
    const uint32_t w = bus_device_read32(9, base);
    bus_device_write32(9, base, (w & ~(0xFFu << shift)) | (uint32_t(v) << shift));
}


/* The corrections themselves, factored out so the build-time call and the
 * per-frame call cannot drift apart - two copies of a hiding rule is exactly
 * how one of them keeps being right while the other is the one that runs. */
void pause_apply(uint32_t obj) {

    /* THE DS ICON, which is a separate widget and not part of the label.
     *
     * The builder's own wireless branches give it away - each one writes bit 0
     * of `[[obj+0x30] + 8]` alongside choosing row 1's text, setting it in the
     * case that leaves the row reading "Host Wireless" and clearing it in the
     * two that rename the row. A flag that tracks "is this row the wireless
     * one" is the flag that draws the wireless icon, and bit 0 is the same
     * present bit the party code uses on actors. */
    const uint32_t icon = bus_device_read32(9, obj + kPauseIconField);
    if (icon >= 0x02000000u && icon < 0x02400000u) {
        const uint32_t flags = bus_device_read32(9, icon + 8u);
        if (flags & 1u) {
            bus_device_write32(9, icon + 8u, flags & ~1u);
            if (!g_pause_reported) {
                g_pause_reported = true;
                /* AND THE TWO BYTES QUIT GAME BRANCHES ON, reported from the
                 * same one-shot line because this is the only place the fork
                 * ever holds this object.
                 *
                 * Reading `0x0208EF00`: `[obj+0xE3]` chooses
                 * between quitting the whole game and leaving a session, and
                 * `[obj+0xE1]` gates the leave itself. Online play wants row 5 to read
                 * "Disconnect" / "Shut Down Lobby" while connected, and whether
                 * the cartridge's own leave path is reachable from the fork -
                 * rather than reimplemented - depends entirely on these two.
                 *
                 * One word, because +0xE0..+0xE3 share it and reading four
                 * bytes as four bytes is a width mistake this project has
                 * made repeatedly. */
                const uint32_t e0 = bus_device_read32(9, obj + 0xE0u);
                std::fprintf(stderr,
                             "[fork] pause menu: cleared the wireless icon at "
                             "%08X (menu %08X)  +0xE1=%u +0xE3=%u\n",
                             unsigned(icon), unsigned(obj),
                             unsigned((e0 >> 8) & 0xFFu),
                             unsigned((e0 >> 24) & 0xFFu));
                /* THE SECOND ACTIVATION ARM, unclosed and therefore reported.
                 * `[record + 0x02] >= 0x0A` activates a row on a tap WITHOUT
                 * consulting the cursor. If any of these three prints 10 or
                 * more, that row is still reachable by touch and needs a fix
                 * that is not a guess about what this halfword means. */
                for (uint32_t row : kLsw2PauseHiddenRows) {
                    const uint32_t rec =
                        obj + kPauseRowRecords + row * kPauseRowStride;
                    const uint32_t lo = read_guest_byte(rec + kPauseRowTouchKind);
                    const uint32_t hi =
                        read_guest_byte(rec + kPauseRowTouchKind + 1u);
                    std::fprintf(stderr,
                                 "[fork] pause row %u: touch-kind %u %s\n",
                                 unsigned(row), unsigned(lo | (hi << 8)),
                                 (lo | (hi << 8)) >= 0x0A
                                     ? "(>= 10: STILL TAPPABLE)"
                                     : "(below 10: closed)");
                }
            }
        }
    }

    /* THE TOUCH PATH, closed on the same three rows the cursor already skips.
     * See the note above kPauseRowRecords for why the cursor nudge alone was
     * not enough and why only the enabled byte is written. */
    for (uint32_t row : kLsw2PauseHiddenRows) {
        const uint32_t rec = obj + kPauseRowRecords + row * kPauseRowStride;
        if (read_guest_byte(rec + kPauseRowEnabled) != 0) {
            write_guest_byte(rec + kPauseRowEnabled, 0);
            /* SAY SO, ONCE PER ROW PER MENU. The row hiding sat unverified for
             * two weeks partly because nothing here ever printed: a drive could
             * open the pause menu and photograph it, but had no line to gate on
             * that fires when the hiding WORKS - only the picture, which says
             * "no Extras row" identically whether the row was hidden or the menu
             * never opened. Printed on the transition from enabled to disabled,
             * so it is the event and not the steady state. */
            static uint32_t said_obj = 0; static uint32_t said_mask = 0;
            if (said_obj != obj) { said_obj = obj; said_mask = 0; }
            if (!(said_mask & (1u << row))) {
                said_mask |= (1u << row);
                std::fprintf(stderr, "[fork] pause: disabled row %u (hidden - "
                             "Host Wireless / Extras / Save Game)\n", row);
            }
        }
    }

    /* THE CURSOR, moved off a hidden row IN THE DIRECTION IT WAS ALREADY
     * GOING. Navigation must move to the next visible item,
     * and next depends on which way the player pressed - stepping forward out
     * of a row they reached by pressing up would fight them back down it. The
     * previous resting index is the only direction signal available here, and
     * it is enough: the cursor only ever moves one row per press. */
    uint32_t cursor = bus_device_read32(9, obj + kPauseCursorField);
    const uint32_t from = cursor;
    bool corrected = false;
    if (cursor < kPauseRowCount && lsw2_pause_row_hidden(cursor)) {
        corrected = true;
        const uint32_t next = lsw2_pause_escape(cursor, g_pause_last_cursor);
        bus_device_write32(9, obj + kPauseCursorField, next);
        cursor = next;

        /* AND ASK THE GAME TO REDRAW, because this write is happening INSIDE
         * the builder's row loop and the highlight is decided in the same pass.
         * That timing - not the arithmetic above - is why "up onto Resume"
         * never highlighted while "down onto Resume" always did: going down
         * needs no correction, so the game's own rebuild draws it. See
         * pauseredraw.h. */
        lsw2_pause_request_redraw(obj);
    }

    /* REMEMBER ONLY WHERE THE PLAYER CAME TO REST, not where we put them.
     *
     * Moving UP onto Resume did not highlight it, while wrapping DOWN onto
     * Resume did. Down onto Resume works because it needs no correction:
     * the guest wraps 5 -> 0 and 0 is visible. Up onto Resume goes 2 -> 1, which
     * IS hidden, so it is the corrected path that misbehaves.
     *
     * The direction signal is "which row the cursor was on before this press".
     * Storing the CORRECTED value made that a record of our own writes rather
     * than of the player's movement, so on the next press the comparison was
     * against a position the player had never chosen.
     *
     * AND IT IS LOGGED, once per distinct correction. Three attempts at this
     * menu have now been reasoned out and shipped, and the last one could not
     * execute at all; a rule about which way the cursor jumped is exactly the
     * kind of thing that should be read from a run rather than argued about. */
    if (corrected) {
        static uint32_t said_from = 0xFFFFFFFFu, said_to = 0xFFFFFFFFu;
        if (from != said_from || cursor != said_to) {
            said_from = from;
            said_to = cursor;
            std::fprintf(stderr,
                         "[fork] pause cursor %u (hidden) -> %u, %s from %u\n",
                         unsigned(from), unsigned(cursor),
                         from < g_pause_last_cursor ? "up" : "down",
                         unsigned(g_pause_last_cursor));
        }
    } else {
        g_pause_last_cursor = cursor;
    }
}

/* The build-time entry: learn the object, learn which state owns it, correct
 * it once now so the very first frame is already right. */
void pause_menu_fixups() {
    const uint32_t obj = g_cpu.R[10];
    if (obj < 0x02000000u || obj >= 0x02400000u) return;

    /* WHAT THIS ROW ACTUALLY SAYS, recorded before anything is decided.
     *
     * This override replaces `0x02066708`, and the pause loop calls it at
     * `0x0208E9C8` with `MOV r0, r9` on the instruction before - where r9 came
     * from `LDRH r9, [r11 + row*2]`, the builder's own array of string ids. So
     * the id is sitting in R[0] every time round, and it is the only place the
     * port can see it: the row records at `menu + 0x98` are 12 bytes of
     * hit-test rectangle with no id in them.
     *
     * Recording it first matters - lsw2_pause_row_y below asks which rows are
     * hidden, and we would rather it answered by identity than by position. */
    const uint32_t row = g_cpu.R[6];
    lsw2_pause_note_row_id(row, g_cpu.R[0]);

    /* RE-CENTRE THIS ROW, every time round the loop. r6 is the row index and r7
     * is the Y the builder is about to draw at - see the note above
     * kLsw2PauseFirstY for why writing it here is enough. */
    if (row < kPauseRowCount) g_cpu.R[7] = lsw2_pause_row_y(row);

    g_pause_obj = obj;
    for (int slot = 0; slot < 2; ++slot)
        g_pause_kind[slot] = lsw2_cheats_state_kind(slot);

    pause_apply(obj);
}

/* Does the menu we remembered still exist?
 *
 * DELIBERATELY CONSERVATIVE, because the failure modes are not symmetric.
 * Stopping too early costs the cursor nudge until the menu is rebuilt, which is
 * the bug we already had. Stopping too LATE means writing bytes into an object
 * the game has freed and reused - a hiding fix that becomes a corruption bug,
 * which is far worse than the thing it was fixing.
 *
 * Two independent tests, both cheap:
 *
 *   the STATE, both slots, exactly as they read when the menu was built. Any
 *   change means the stack moved and this object is no longer the live menu.
 *
 *   the STRUCTURE, because a state kind can repeat: `+0x30` must still hold a
 *   RAM pointer (the icon widget) and the cursor must still be a valid row
 *   index. Freed memory passes neither for long. */
bool pause_menu_still_live() {
    const uint32_t obj = g_pause_obj;
    if (obj < 0x02000000u || obj >= 0x02400000u) return false;

    for (int slot = 0; slot < 2; ++slot)
        if (lsw2_cheats_state_kind(slot) != g_pause_kind[slot]) return false;

    const uint32_t icon = bus_device_read32(9, obj + kPauseIconField);
    if (icon < 0x02000000u || icon >= 0x02400000u) return false;

    return bus_device_read32(9, obj + kPauseCursorField) < kPauseRowCount;
}

/* 0x02086534 IS NOT OVERRIDDEN, AND THE REASON IS THE POINT.
 *
 * It was, briefly. 0x020687B4 - the Free Play select's tap handler - keeps the
 * widget in r6 and calls this twice on its mode-0 path, so an lr test there
 * would hand the fork the exact widget the player is touching. That is a real
 * mechanism and the addresses are kept below.
 *
 * IT IS NOT NEEDED, because the widget was never wrong. The constructor's
 * capture has been correct all along:
 *
 *     [fork] select widget pointer 00000000 -> 021FDFA8   (once, at the ctor)
 *     [fork] free play select: page 6, entry 4/5/6/7      (tracking every tap)
 *     socket read of 021FDFA8: vtable 020FE790, page 6, entry 0x07
 *
 * THE "WRONG WIDGET" WAS AN ARTIFACT OF THREE EMULATORS. A socket scan reported
 * a different live widget (021DFA04) than the log did (021FDFA8), and that
 * disagreement drove two iterations of scanning, a windowed search, a vtable
 * hunt and a read-path theory. All of it dissolved once `taskkill` left exactly
 * ONE process running: stale instances were sharing debug port 19842, so the
 * scan and the log were describing different games.
 *
 * SO THE OVERRIDE WAS REMOVED RATHER THAN KEPT AS INSURANCE. It reproduces a
 * function with 104 call sites faithfully, which is work and risk, and it
 * bought nothing measurable - the negative test showed the selection tracked
 * just as well without it. Adding cost to a hot path for a problem that does
 * not exist is how a port gets slow for reasons nobody can later explain.
 *
 * If the pointer ever DOES go stale - the Character tab rebuilding the widget
 * is still a plausible way for that to happen - the two addresses below are
 * where to catch it, and this note is the recipe. */


void flag_test_array_c() {
    /* THE GEAR HOP, OFFERED TO EVERY HOOK THAT RELIABLY FIRES.
     *
     * WHY MORE THAN ONE. 0x0202C218 was measured at sixty calls a second and
     * hosted the hop alone - and then in a later build it stopped being
     * dispatched at all, with the hop silently inert and customization after a
     * Free Play switch quietly broken again. The cause of that is not yet
     * understood, which is exactly why the hop must not depend on any single host.
     *
     * THIS IS SAFE BY CONSTRUCTION: the hop records WHICH host armed it and
     * only that host may complete it, so offering it to several costs nothing
     * and the first one to fire drains the queue.
     *
     * These three are dispatched in EVERY run this project has logged. They are
     * rarer than once a frame, which was why they were rejected as hosts for a
     * per-frame need - but applying a customization is not a per-frame need. A
     * hat that arrives a second late is invisible; a hat that never arrives is
     * the bug. */
    if (lsw2_gearhop_hook(0x02086204u)) return;

    lsw2_probe_hook_rate("flag_test_array_c");
    /* THE TOP-SCREEN PORTRAIT'S DEFERRED REDRAW (after a Free Play character
     * change), DRAINED FROM INSIDE GUEST EXECUTION.
     *
     * Not this function's business, and here anyway for one reason: it runs
     * while the scheduler loop is live, which is the single thing the redraw
     * needs and the fork tick cannot offer. redraw_page copies portrait tiles
     * by hardware DMA and a between-frames call stalls on the transfer -
     * guestcall.h carries the disassembly.
     *
     * SILENT AND FREE WHEN NOTHING IS PENDING - one bool. Placed at the top so
     * it is not skipped by any of the early returns below. */
    lsw2_partyportrait_redraw_if_pending();

    /* THE VIEW-MANAGER HOP, here for the same reason and not by coincidence.
     *
     * State 54 - the map/gamemode chooser, and the fork's doorway into the
     * cartridge's own arena multiplayer - makes a virtual call through the view
     * manager at 0x021092F4 and the fork has none, so it lands in the BIOS.
     * The cartridge builds that object in FUN_0207281C, whose
     * wireless bring-up waits on the ARM7 over PXI, and a call from the fork
     * tick advances only the ARM9 - measured, with the spin's own PC
     * (whsession.h). This override runs while the scheduler loop is live, so
     * handing the guest's PC to the allocator from HERE lets both cores run
     * and the handshake complete.
     *
     * THIS FUNCTION IS PURE, WHICH IS THE PRECONDITION. The hop defers its
     * call by one dispatch and then repeats it; a host with side effects would
     * do them twice. A bit test does none.
     *
     * FIRST, so no early return below can skip it. */
    if (lsw2_wh_trampoline_hop(kFlagTestArrayC)) return;

    const uint32_t base = g_cpu.R[0];
    const uint32_t index = g_cpu.R[1];

    /* NAME EVERY DISTINCT FLAG THE ARENA LOBBY TESTS.
     *
     * "You must Save before doing that!" was still on screen, and the right
     * shape of fix is to stop trying to intercept the popup and make the game
     * believe it has saved.
     *
     * To do that the gate has to be identified, and it is a level SCRIPT
     * condition rather than code (the id 0x2D7 appears nowhere in the ARM9
     * image at any alignment). This function is the flag test the fork already
     * replaces, so if the condition runs through it, the index shows up here.
     *
     * Deduped and capped: a few lines once, not a flood. If the console
     * interaction produces no new line at all, the gate does not use this
     * function and the search moves elsewhere - which is itself the answer. */
    /* IN THE BASELINE TOO, on every level, when LSW2_HLE=1 asks for it.
     *
     * The fork's version of this log named the save gate (flag 11) after three
     * wrong guesses at the popup. The Cantina hub's console entity, needed for
     * the master server's baseline lobby list, is the same shape of question,
     * and
     * `config/strings.json` confirms the dialog: 0x2D6, "Do you want to use DS
     * Wireless?".
     *
     * That id will not appear in the image, exactly as 0x2D7 did not: it is a
     * script argument. What CAN be seen is the branch that decides to raise it,
     * and if it goes through this flag test the index shows up here. The
     * baseline only installs overrides under LSW2_HLE=1, so that is the run
     * to read it from.
     *
     * A DEAD LEAD RECORDED SO IT IS NOT RE-WALKED: the twelve callers of the
     * dialog function 0x020601AC include two that reference the wireless owner,
     * at 0x02069688 and 0x020696C4. They look promising and are not - their
     * dialog ids are 0x23 and 0x24, which strings.json names as the DS Card
     * read/write errors. That module is the cartridge error handler. */
    if (lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel || !ui_fork_build()) {
        static uint32_t seen[32] = {0};
        static int seen_count = 0;
        bool known = false;
        for (int i = 0; i < seen_count; ++i) if (seen[i] == index) known = true;
        if (!known && seen_count < 32) {
            seen[seen_count++] = index;
            std::fprintf(stderr,
                         "[hle] flag test: index %u (base %08X) on level %d\n",
                         unsigned(index), unsigned(base),
                         lsw2_cheats_current_level());
        }
    }
    const uint32_t word_addr =
        base + ((index >> 5) * 4u) + 0x4000u + 0x488u;
    const uint32_t word = bus_device_read32(9, word_addr);
    const uint32_t bit = index & 31u;
    uint32_t result = (word & (1u << bit)) ? 1u : 0u;

    /* THE LOBBY'S FREE PLAY ROSTER, and this is why the seam was worth proving.
     *
     * The Free Play widget's constructor decides party mode versus roster mode
     * by asking this very function for bit 10. Writing that bit into
     * the array does not work - the game reinitialises the array on every level
     * load, which was measured twice - but ANSWERING the question differently
     * does, because the constructor never reads the array itself.
     *
     * Scoped as tightly as it can be: the fork only, this base only, this index
     * only. Every other caller of this function gets the guest's own answer.
     *
     * This changes what the game computes, which is over the line
     * port/ARCHITECTURE.md draws for the host layer - flagged here at the code
     * that does it, as that document requires. */
    /* AND ONLY IN THE ARENA LOBBY: the roster is displayed only while in the
     * Arena lobby map.
     *
     * A line-by-line re-read of the requirement found this condition missing entirely - the
     * forcing was scoped to the fork, the base and the index, and to nothing
     * about where the player is. Nobody had noticed because the fork cannot
     * leave the lobby yet, so every call so far HAS been in it; the moment
     * Phase D makes a match reachable it would have shown the Free Play roster
     * in the middle of one.
     *
     * That is the shape of bug this project keeps producing: correct behaviour
     * that is correct only because the situation that breaks it does not exist
     * yet. Cheaper to add the condition than to remember why it was safe. */
    const bool forced = g_force_free_play && lsw2_hle_fork_wants_roster() &&
                        base == 0x02104D90u && index == 10u &&
                        lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel;
    if (forced) result = 1u;

    /* THE LOBBY CONSOLE'S SAVE GATE - AND WHY FORCING FLAG 11 WAS WRONG.
     *
     * Forcing index 11 to 1 did not open the console. It
     * changed the refusal from 0x2D7 "You must Save before doing that!" to
     * **0x2D5 "You can't do that during a Wireless game!"**.
     *
     * So flag 11 is not "has saved". It is "a WIRELESS GAME IS ACTIVE", and the
     * record at `0x02057084` branches on it into one refusal or the other:
     *
     *     flag 11 == 1  ->  0x2D5, you are already in a wireless game
     *     flag 11 == 0  ->  0x2D7, you have not saved
     *
     * BOTH ARMS REFUSE. There is no value of flag 11 that opens this console, so
     * no amount of answering it differently was ever going to work - which is
     * the same shape of error as the two popup waivers before it: a lever that
     * moves, attached to nothing that opens.
     *
     * The console is opened from the string decode instead, which a trace
     * proved DOES see this - `[trace] id 0x2D7` came out of that seam.
     * See kConsoleRefusalIds in the string override. */
    /* The first dozen calls, with their indices. One line was enough to prove
     * the seam is reached; it is not enough to tell WHICH caller was reached,
     * and that turned out to be the question that mattered. */
    if (g_flag_hits < 24u) {
        /* THE RETURN ADDRESS NAMES THE CALLER, which is the whole question.
         * Five calls with index 10 could be five sites or one site five times,
         * and only one caller matters: the Free Play constructor, which returns
         * to 0x02068384. If that address never appears, the constructor is not
         * reaching this override and everything else is beside the point. */
        /* R4 IS THE OBJECT, when the caller is the constructor. 0x020682C4
         * keeps the widget it is building in r4 throughout - it is what every
         * store in that function writes through - so logging it here says
         * exactly which object this construction is for. If that address is not
         * the widget the lobby ends up showing, then the forced answer went to
         * a different object and the whole contradiction dissolves. */
        std::fprintf(stderr,
                     "[hle] flag-test #%u from %08X r4=%08X base %08X index %u"
                     " -> %u%s\n",
                     unsigned(g_flag_hits), unsigned(g_cpu.R[14]),
                     unsigned(g_cpu.R[4]), unsigned(base), unsigned(index),
                     unsigned(result),
                     forced ? "  FORCED" : "");
    }
    /* THE LOBBY ROSTER'S BOUNTY-HUNTER PAGE RESTRICTION, applied at the one
     * instant it is free.
     *
     * The character select's page bounds were established to be not
     * immediates but PER-INSTANCE FIELDS - `+0x24` low, `+0x28` high-exclusive -
     * and that the game's wrap rule is written purely in terms of them: left
     * from the low bound goes to high-1, right from high-1 goes to low. So
     * bounds of [6, 8) give cycling between the two pages even when going only
     * right or only left, with nothing else to write. The Bounty Hunters are
     * pages 6 and 7, from the page table's own titles.
     *
     * What was missing was the live object's address, which had been assumed
     * to need a runtime search. It is not one. THE CONSTRUCTOR ASKS THIS OVERRIDE A
     * QUESTION, and it holds the widget in r4 while doing so:
     *
     *     02068318  str   r3, [r4, #0x24]     low  = 0
     *     02068320  str   r2, [r4, #0x28]     high = 12
     *     02068350..02068374                  clamp the remembered page
     *     02068380  bl    0x02086204          <- we are here, lr = 0x02068384
     *     02068394  bl    0x0206846C          build the portrait widgets
     *
     * The bounds are already final and the portraits are NOT yet built, so this
     * is the last moment at which changing them costs nothing and the first at
     * which the object is reachable. One equality test on lr is what separates
     * it from the customize screen, which asks the same question from
     * 0x0207AA30 and must keep the full range.
     *
     * THE PAGE IS RE-CLAMPED because the constructor clamped it against the old
     * bounds: a player last on "Jedi Page 2" would otherwise arrive on a page
     * outside the new range, which the paging code has no reason to correct. */
    /* A CONFLICT WITH THE HOST CHARACTER PICKER THAT WAS RECORDED HERE AS
     * "NOT REAL". IT IS REAL.
     *
     * === SUPERSEDED, KEPT SO NOBODY RE-DERIVES IT =========================
     * | The previous pass reasoned that the online host's character picker - |
     * | a Free Play-style menu offering every playable character, unlikely   |
     * | ones such as R5-D4 and Gonk Droid included - would build this widget |
     * | in this same level, where this clamp would cut it to two pages.      |
     * |                                                                      |
     * | It does not. That screen is a pseudo-Free-Play menu that confirms    |
     * | on **A**; this widget's three button tests read as L, R and B, with  |
     * | confirmation by double tap. A host pressing A on the cartridge's     |
     * | widget would do nothing at all. So the picker is the fork's own menu |
     * | - characterpick.{h,cpp} - and never reaches here.                    |
     * ======================================================================
     *
     * IT WAS SETTLED THE OTHER WAY: the picker IS the Free Play touch-screen
     * menu, behaving and responding to touch exactly as Free Play does. Both
     * observations above are still true and the conclusion drawn from them
     * was wrong: the double tap is not evidence against this widget, it IS
     * the picker's second way to confirm (A, or a double tap on the
     * character's icon).
     *
     * So the conflict this note talked itself out of is the live one, and the
     * clamp below could not resolve it on its own - it runs at construction,
     * once, and the host opens the Character row long afterwards. The range is
     * maintained per frame in fork.cpp now; this call sets the opening one. */
    /* THE TOP-SCREEN PORTRAIT FIX NEEDS THIS POINTER AND NEEDS IT IN BOTH
     * BUILDS.
     *
     * The forcing below is the lobby roster's and is fork-only by design, but the widget
     * pointer it reads is the same one `party_refresh` and `redraw_page` take,
     * and the portrait bug is the cartridge's - so the capture is separate from
     * the forcing and has no conditions on it beyond the caller being the Free
     * Play constructor. See partyportrait.h.
     *
     * `lr == 0x02068384` is what identifies that caller, and nothing else
     * reaches this override with it - the note above kFreePlayCtorReturn says
     * why, and the customize screen's own question arrives from 0x0207AA30. */
    /* BRACED, AND THE BRACES ARE THE WHOLE POINT. This was a one-statement `if`
     * while only the portrait fix wanted the pointer. Adding the second call below it left
     * that call OUTSIDE the condition with the indentation saying otherwise, so
     * lsw2_selectwidget_note ran on EVERY save-flag query with whatever a
     * callee-saved register happened to hold. Its main-RAM filter does not save
     * it: any plausible-looking word would have been stored as "the widget" and
     * the host's pick read out of an object that is not one. tools/sweep_braces
     * .py exists because of this line. */
    /* AND THE WIDGET THE PLAYER IS ACTUALLY TAPPING, which is a different one.
     *
     * The capture below fires at the Free Play CONSTRUCTOR and happens once.
     * Measured: the widget the player taps is a different object -
     * the fork held 021FDFA8 while the live one was 021DFA04 - so the picker
     * read `entry -1` off a dead object while a portrait was visibly selected.
     * Two scans were tried to find it and both failed; this needs no search.
     *
     * THE TAP HANDLER ASKS THIS SAME QUESTION. 0x020687B4 keeps the widget in
     * r6 for its whole length, and on the mode-0 path - which is the Free Play
     * select's path, `[r6+0x35] == 0` - it calls the save-flag query twice to
     * ask whether a character is unlocked:
     *
     *     0206887C  bl 0x02086534   -> returns to 02068880
     *     020688B4  bl 0x02086534   -> returns to 020688B8
     *
     * So every tap on a portrait comes through here with the RIGHT widget in
     * r6. One equality test on lr, the same trick the constructor capture
     * already uses, and the pointer is correct by the game's own doing rather
     * than by anything the fork has to find.
     *
     * THERE IS NO GLOBAL TO READ INSTEAD, which was checked first: the paging
     * code stores the page at [0x02104D90 + 0x51] but the SELECTION never
     * leaves the object - the mode-0 path reads `[r6+0x36]` directly and writes
     * it nowhere else. The widget really is the only place it lives.
     *
     * AND IT CANNOT BE DONE FROM HERE, which sweep_lr_checks caught within a
     * minute of it being written and was entirely right to. THIS override is on
     * 0x02086204; the tap handler calls 0x02086534. Different function, so an
     * lr test for 02068880 / 020688B8 in this handler can never match - dead
     * code that reads like a fix.
     *
     * IT ALSO APPEARED TO WORK, which is the part worth recording. With the
     * dead check in place a run reported `page 6, entry 6/7/4/5` - the fork
     * tracking every tap, exactly the result being chased. It was the heap:
     * a rebuild changes the allocation order, and on that run the
     * constructor's capture happened to be the widget the player was using.
     * A green result from dead code is indistinguishable from a fix, and the
     * only reason it was caught is that a gate contradicted it.
     *
     * TO ACTUALLY DO IT: 0x02086534 needs an override of its own. It has 104
     * BL sites with only 4 direct calls in the generated code, so an override
     * there is reached by essentially all of them - but it is a hot function
     * and a faithful replacement is real work, so it is not started here. A
     * pure observer that delegates would be the shape (statereq.cpp's), with
     * the caveat that statereq has never produced a line and that is unexplained.
     */

    if (g_cpu.R[14] == kFreePlayCtorReturn) {
        lsw2_partyportrait_note_widget(g_cpu.R[4]);
        /* AND THE SAME POINTER FOR THE ONLINE HOST'S CHARACTER PICKER, which
         * is this very widget. Two notes because the two want different lifetimes
         * of one address - see selectwidget.cpp. */
        lsw2_selectwidget_note(g_cpu.R[4]);
    }

    /* THROUGH selectwidget.cpp, not with four writes of its own. This block
     * used to hold the only copy of "clamp the widget to a page range", and
     * fork.cpp's per-frame maintenance would have been a second copy of the
     * same four accesses with the same re-clamp rule - which is how a pair of
     * writers ends up disagreeing about whether the high bound is exclusive.
     * The pointer was noted just above, so the object is already reachable. */
    /* STILL GATED ON `forced`, NOT ONLY ON THE RANGE POLICY. `forced` also
     * pins the query itself - block 0x02104D90, index 10 - and the lr test
     * identifies the caller. Both were required before and the policy call
     * replaces neither: it answers "what range", not "is this the right
     * moment". Weakening a condition while refactoring is how a clamp starts
     * firing on the customize screen. */
    unsigned range_low = 0, range_high = 0;
    if (forced && g_cpu.R[14] == kFreePlayCtorReturn &&
        lsw2_hle_fork_select_range(&range_low, &range_high)) {
        lsw2_selectwidget_set_pages(range_low, range_high);
        if (g_bounds_hits == 0)
            std::fprintf(stderr,
                         "[fork] character select opens on pages %u-%u "
                         "(widget %08X)\n",
                         range_low, range_high - 1, unsigned(g_cpu.R[4]));
        ++g_bounds_hits;
    }

    ++g_flag_hits;
    /* RETURN `result`, NOT THE ARRAY. This line used to recompute the bit test
     * and hand that back, which threw the forcing above away - it reached the
     * log line and nothing else, so the lobby's roster mode had never actually
     * been forced since it was written. Found while reading this function for a
     * different reason: the customize screen picks its row table on this very
     * flag, and twenty-nine rows versus twelve is the difference between all
     * customization options and a subset of them.
     *
     * The failure is worth naming because it is invisible from both ends. The
     * log said FORCED, so the trace looked right; the game got the unforced
     * answer, so the behaviour looked like the forcing being ineffective rather
     * than absent - which is a much harder thing to go looking for. */
    g_cpu.R[0] = result;
    g_cpu.R[15] = g_cpu.R[14];
}

/* ONLINE PLAY: the guest's data-sharing, answered by our session.
 *
 * These two are a pair and only make sense together. The short form of the
 * argument is that the receive buffer at 0x0210B520 is
 * referenced from exactly two places in the whole image, and both of them are
 * these functions. The game never reads it by address - it asks
 * WH_GetSharedDataAddress for a pointer.
 *
 * SO WE CHOOSE THE LAYOUT. Because the only view the guest has of that memory
 * is the pointer we hand back, the Nitro WMDataSet structure never has to be
 * reproduced. `0x0210B520 + aid * 124` is ours to define, and nothing else in
 * the image touches it.
 *
 * BOTH ARE VIRTUAL CALLS, which is why a caller scan finds nothing for either.
 * The WH layer is a C++ object whose vtable is 0x020FFD50, held at
 * 0x0210B320+0x28, and every method ignores r0 because r0 is `this` - step is
 * slot 6 of that table and get-address is slot 9.
 * Address dispatch still catches them: an indirect branch resolves by target
 * address, and that is what the table below is keyed on.
 *
 * THIS DISPLACES THE VENDORED WIRELESS MODEL when LSW2_HLE=1. The runner does
 * carry a melonDS Wifi device, so the guest's own WM path is not necessarily
 * dead - it is being deliberately bypassed in favour of a modern network
 * transport. That is a real trade and it is why the whole file is
 * opt-in.
 */
/* THE SAME OBJECT AS ABOVE, and it was declared twice in this file - see
 * viewmask.h, which now owns the base and all three fields. */
constexpr uint32_t kWhRecvBase = kLsw2WhSharedData;
constexpr int kShareBytes = 124;         /* dataLength, both WM_StartDataSharing sites */
constexpr int kMaxStations = 4;          /* aidBitmap 0xF */
constexpr uint32_t kWmErrNoDataset = 5u;

static_assert(kShareBytes % 4 == 0,
              "the block is copied word-at-a-time; the bus has no 8-bit device "
              "write, and 124 divides by 4 so it never needed one");

uint64_t g_step_hits = 0;
uint64_t g_step_stalls = 0;

/* Mirror the guest's own error recording, read out of
 * 0x020AD8B4: store the code unless the module is already in ERROR. It does
 * nothing else - no state change, no teardown - which is what makes returning
 * "no data" a safe answer rather than a destructive one. */
void wh_record_errcode(uint32_t code) {
    if (bus_device_read32(9, kLsw2SessionAuthority + kLsw2WhSystemState) != 9u)
        bus_device_write32(9, kLsw2SessionAuthority + kLsw2WhErrorCode, code);
}

void wh_step_data_sharing() {
    const uint32_t send_buf = g_cpu.R[1];  /* r0 is `this`, unused */
    ++g_step_hits;

    Lsw2LockstepSession* session = lsw2_net_active_session();
    if (!session) {
        /* No networked game. Report the frame the way the radio would when it
         * has nothing - see lsw2_net_bind.cpp. */
        wh_record_errcode(kWmErrNoDataset);
        g_cpu.R[0] = 0;
        g_cpu.R[15] = g_cpu.R[14];
        return;
    }

    /* THE GUEST'S BLOCK. Read 124 bytes, not the 544 the guest flushes:
     * WHCTX+0x3C is the send BUFFER size and the game's own log string says so
     * (an earlier reading got exactly this wrong). Only dataLength is
     * transmitted, and dataLength is 124 at both WM_StartDataSharing sites. */
    uint8_t local[kShareBytes];
    for (int i = 0; i < kShareBytes; i += 4) {
        const uint32_t w = bus_device_read32(9, send_buf + uint32_t(i));
        local[i + 0] = uint8_t(w);
        local[i + 1] = uint8_t(w >> 8);
        local[i + 2] = uint8_t(w >> 16);
        local[i + 3] = uint8_t(w >> 24);
    }

    uint8_t blocks[kMaxStations][kShareBytes];
    uint16_t bitmap = 0;
    if (!session->step(local, blocks, &bitmap)) {
        /* A station's block for the frame being consumed has not arrived. The
         * guest MUST NOT advance on this - which is precisely what returning 0
         * tells it, and it is the same 0 the guest gives itself on NO_DATASET. */
        ++g_step_stalls;
        wh_record_errcode(kWmErrNoDataset);
        g_cpu.R[0] = 0;
        g_cpu.R[15] = g_cpu.R[14];
        return;
    }

    for (int station = 0; station < kMaxStations; ++station) {
        const uint32_t base = kWhRecvBase + uint32_t(station * kShareBytes);
        for (int i = 0; i < kShareBytes; i += 4) {
            const uint8_t* b = &blocks[station][i];
            bus_device_write32(9, base + uint32_t(i),
                               uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
                                   (uint32_t(b[2]) << 16) |
                                   (uint32_t(b[3]) << 24));
        }
    }

    g_cpu.R[0] = 1;
    g_cpu.R[15] = g_cpu.R[14];
}

void wh_get_shared_data_address() {
    const uint32_t aid = g_cpu.R[1] & 0xFFFFu;  /* r0 is `this`, unused */
    /* Out of range returns null, which is what WM_GetSharedDataAddress does for
     * a station outside the bitmap. Four is the arena's own width. */
    g_cpu.R[0] = (aid < uint32_t(kMaxStations))
                     ? (kWhRecvBase + aid * uint32_t(kShareBytes))
                     : 0u;
    g_cpu.R[15] = g_cpu.R[14];
}

/* THE STRING BANK, so the fork can say its own things in the game's own menus.
 *
 * The fork's design puts almost every fork control in the game
 * rather than in imgui, and the cheapest way to change what a menu entry SAYS is
 * to answer its string id differently. The three arena menu
 * entries the fork replaces already exist as ids 0x63, 0x64 and 0x65,
 * so this is a relabel rather than a new menu.
 *
 * WHY REPLACING IS SAFE HERE, when the seam cannot wrap. An override owes the
 * guest a correct answer for every id it does NOT replace, and that is why this
 * one waited: `port/frontend/strings.cpp` had to be checked against the real
 * bank first. It has been - `test_strings_real.cpp` decodes all 1333 strings
 * from a rebuilt-RAM fixture with zero mismatches and zero two-byte glyphs.
 *
 * The guest's contract, reproduced exactly:
 *   r0 = id, r1 = destination, r2 = capacity
 *   returns 0 ok, 1 the bank is not loaded yet, 2 the buffer filled first
 */
uint64_t g_string_hits = 0;

uint8_t guest_read8(uint32_t addr, void*) {
    return bus_debug_read8(9, addr);
}

/* ------------------------------------------------------------------------- */
/* WHO IS SHOWING THIS TEXT? - a locator, because for some strings the ROM will
 * not say.
 *
 * WHY THIS HAD TO EXIST. The Arena Lobby's console entity was found
 * refusing to open with "You must Save before doing that!", and the fork
 * waives that gate. Finding a gate means finding the code
 * that shows the message, and the usual route - look up the string id, find who
 * passes it - dead-ends completely here:
 *
 *   - the id is 0x2D7, and the word 0x000002D7 does not appear ANYWHERE in the
 *     ARM9 image, at any alignment, in either section. It is not an immediate
 *     (it needs ten bits) and it is not in a literal pool.
 *   - every string in the game is fetched through 0x02066708, which has 108
 *     call sites. Sixty-five of them pass a register. Evaluating the other
 *     forty-three, and every `base + index` chain among the sixty-five, gives a
 *     highest reachable id of 0x2AA. Nothing static can produce 0x2D7.
 *
 * So the id is assembled from data at runtime, and the only place it is
 * certainly visible is the moment the string is decoded - which is here, and
 * only here: 0x0205CD20 has exactly one caller in the whole image.
 *
 * SET LSW2_TRACE_TEXT TO A SUBSTRING. Every decoded string containing it is
 * reported with the id and a guest-stack backtrace, so one reproduction names
 * the caller:
 *
 *     LSW2_TRACE_TEXT="must Save"  ./lsw2_bountyhunter
 *
 * THE BACKTRACE IS A FILTERED STACK SCAN, not a frame walk. The guest's frames
 * are not described anywhere the port can read, so this takes every word on the
 * stack that looks like an ARM9 code address and keeps the ones with a `BL`
 * immediately before them - which is what a return address is. That filter is
 * what makes the output short enough to read: an unfiltered scan of the same
 * window prints mostly pointers and floats.
 *
 * It can still print a stale return address left by an earlier call, so the
 * output is a candidate list to check, not an answer. Checking one is a
 * `show_arm_callers.py` away. */
/* THE FORK TRACES ONE STRING WITHOUT BEING ASKED, and this is deliberate.
 *
 * The console entity's save gate is an OPEN QUESTION with a known reproduction:
 * walk into the console. Making the answer depend on the player remembering to
 * set an environment variable first means the reproduction happens and the
 * evidence does not - which is what already happened once. Defaulting it costs
 * one string comparison per decoded string and prints nothing until the popup
 * actually appears.
 *
 * LSW2_TRACE_TEXT overrides it; LSW2_TRACE_TEXT= (empty) turns it off. */
/* SEMICOLON-SEPARATED. One reproduction should answer every open question it
 * can, because each one costs a rebuild and a play-through. */
constexpr const char* kForkDefaultTrace = "must Save";

/* AND SOME BY ID, because the text is not always available to match on.
 *
 * The fork blanks three pause-menu labels through the string override, so by the time
 * the decoder returns, their text is "" and no substring matches. The id is
 * still the id, and it is what identifies the row - so these are watched
 * directly. The three Extras candidates are all here: they could not be
 * distinguished statically, and whichever one the pause menu asks for will
 * report a stack that says so.
 *
 * The stacks are the real prize rather than the ids. Four refuted attempts
 * were spent on finding who owns the pause row array; the function that
 * asks for "Save Game" is by definition the one building that row. */
const uint32_t kForkWatchIds[] = {
    /* The hidden pause rows. 0x007 is the Extras row, now read out of the menu's own
     * table; 0x15D and 0x161 are the other two static candidates,
     * kept so a stack from either would show up if the reading is wrong. */
    0x007u, 0x02Cu, 0x03Cu, 0x15Du, 0x161u,

    /* AND THE ARENA MENU'S THREE ENTRIES, which answers an open
     * question for free.
     *
     * Five static attempts failed to find which state class shows that menu,
     * and the conclusion was that the cheap version is a runtime one: "log
     * the active state object's vtable while the arena menu is on screen". The
     * tracer does better than that - the stack at the moment those labels are
     * decoded names the drawing function directly, and the fork already
     * relabels exactly these ids, so it knows they belong to that menu.
     *
     * It costs nothing until the menu is drawn. It is not reachable yet - the
     * console entity's save gate is in the way - but the answer will be waiting
     * in the log on the first run where it opens. */
    0x063u, 0x064u, 0x065u,
};

const char* g_trace_text = nullptr;
bool g_trace_checked = false;

bool trace_id_wanted(uint32_t id) {
    if (!ui_fork_build()) return false;
    for (uint32_t w : kForkWatchIds)
        if (w == id) return true;
    return false;
}

bool trace_wanted(const char* text) {
    if (!g_trace_checked) {
        g_trace_checked = true;
        const char* want = std::getenv("LSW2_TRACE_TEXT");
        if (want) {
            if (*want) g_trace_text = want;          /* set: use it        */
        } else if (ui_fork_build()) {
            g_trace_text = kForkDefaultTrace;        /* unset: the default */
        }
    }
    if (!g_trace_text) return false;
    for (const char* p = text; *p; ++p) {
        const char* a = p;
        const char* b = g_trace_text;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

/* An ARM9 main-RAM code address, by the same bounds the rest of the port uses
 * for "is this a pointer into the cartridge's image". */
bool looks_like_code(uint32_t v) {
    return v > 0x02000000u && v < 0x02100000u && (v & 3u) == 0u;
}

/* TO A FILE AS WELL AS TO stderr. A double-clicked exe has no console anyone
 * reads, and asking a player to reproduce a bug from a terminal is asking for
 * the bug not to be reproduced. Appended, opened per report and closed again,
 * so a crash immediately afterwards still leaves the evidence on disk. */
void trace_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);

    /* A BANNER PER RUN, because the file is appended and three runs of
     * three different builds otherwise read as one confusing log. */
    static bool banner = false;
    if (!banner) {
        banner = true;
        if (std::FILE* f = std::fopen("lsw2_trace.log", "a")) {
            std::fprintf(f, "\n=== new run ===\n");
            std::fclose(f);
        }
    }

    if (std::FILE* f = std::fopen("lsw2_trace.log", "a")) {
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fclose(f);
    }
}

/* IS THE WORD AT `addr` A CALL? - which is what makes the address after it a
 * return address rather than a coincidence.
 *
 * THE FIRST VERSION ACCEPTED ONLY `BL`, AND THAT LOST A FRAME. The save-gate
 * backtrace had a 0x68-byte hole between the script dispatcher and the state
 * machine, because whatever ran the script was reached through a vtable - and a
 * `BLX rM` return address has no `BL` in front of it, so the filter threw it
 * away. A gap in a backtrace reads as "nothing there", which is exactly the
 * frame that was wanted.
 *
 * All three call forms, then:
 *   BL   cond 1011 offset          0x?B??????
 *   BLX  1111 101H offset          0xFA/0xFB (unconditional, ARM->Thumb)
 *   BLX  cond 0001 0010 ... 0011 rM  0x?12FFF3?
 */
bool is_call(uint32_t w) {
    const uint32_t cond = (w >> 28) & 0xFu;
    if (cond == 0xFu) return ((w >> 25) & 0x7u) == 0x5u;      /* BLX imm */
    if (((w >> 24) & 0x0Fu) == 0x0Bu) return true;            /* BL      */
    return (w & 0x0FFFFFF0u) == 0x012FFF30u;                  /* BLX reg */
}

void report_guest_stack(uint32_t id, const char* text) {
    /* ONCE PER STRING. The popup is redrawn every frame it is on screen, and
     * sixty identical backtraces a second buries the one that matters. */
    static uint32_t reported[16] = {0};
    static int reported_count = 0;
    for (int i = 0; i < reported_count; ++i)
        if (reported[i] == id) return;
    if (reported_count < int(sizeof reported / sizeof reported[0]))
        reported[reported_count++] = id;

    trace_line("[trace] id 0x%X \"%.60s\"\n", unsigned(id), text);
    trace_line("[trace]   lr = 0x%08X\n", unsigned(g_cpu.R[14]));

    const uint32_t sp = g_cpu.R[13];
    int printed = 0;
    /* 256 words is about 1KB of stack - deep enough to cross the widget and
     * dialog frames that sit between a gate and the decoder, shallow enough
     * not to wander out of the thread's stack entirely. */
    for (uint32_t i = 0; i < 256u && printed < 24; ++i) {
        const uint32_t at = sp + i * 4u;
        const uint32_t v = bus_device_read32(9, at);
        if (!looks_like_code(v)) continue;
        const uint32_t before = bus_device_read32(9, v - 4u);
        if (!is_call(before)) continue;
        trace_line("[trace]   [sp+0x%03X] = 0x%08X  (called from, %s)\n",
                   unsigned(i * 4u), unsigned(v - 4u),
                   ((before >> 24) & 0x0Fu) == 0x0Bu &&
                           ((before >> 28) & 0xFu) != 0xFu
                       ? "BL" : "BLX");
        ++printed;
    }
    if (!printed)
        trace_line("[trace]   no return addresses found on the stack\n");
}

/* THE DECODED-STRING CACHE, and it is a performance fix rather than a tidy-up.
 *
 * The game played choppy on the first build where this override
 * actually ran, and the arithmetic says why. The decoder walks the Huffman tree
 * one bit at a time, and every step reads a CHILD PAIR - `read_u16`, which is
 * two `read8` calls through the guest bus. A forty-character string at ten bits
 * a character is about eight hundred bus reads, and a menu redraws several
 * strings a frame.
 *
 * The lifted body had the whole bank in host memory and paid none of that. Ours
 * goes through the bus for every byte, which is the correct thing to do once and
 * a bad thing to do sixty times a second.
 *
 * CACHING IS SAFE HERE because the bank is immutable once loaded: it is
 * cartridge data copied into RAM at boot, and the decoder refuses to run at all
 * until the control block's data pointer is non-zero. Nothing rewrites it
 * afterwards - if anything did, the guest's own decoder would be equally wrong.
 *
 * Only successful decodes are stored. A NotReady answer means the bank was not
 * up yet and must be asked again; a Truncated one depends on the caller's
 * buffer size, which is not part of the key. */
constexpr uint32_t kStringCacheIds = 1400;   /* the bank holds 1333 */
char* g_string_cache[kStringCacheIds] = {nullptr};
uint32_t g_cache_hits = 0;
uint32_t g_cache_misses = 0;

/* Copy `text` into guest memory the way the guest's own decoder leaves it.
 * A HALFWORD AT A TIME, because the bus exposes no 8-bit device write; the
 * length is rounded up and the terminator is already counted, so the extra byte
 * is either the terminator or past a string we just terminated. */
void write_string_to_guest(uint32_t dst, const char* text, uint32_t total) {
    for (uint32_t i = 0; i < total; i += 2) {
        const uint8_t lo = uint8_t(text[i]);
        const uint8_t hi = (i + 1 < total) ? uint8_t(text[i + 1]) : 0u;
        bus_device_write16(9, dst + i, uint16_t(lo | (uint16_t(hi) << 8)));
    }
}

void string_decode() {
    const uint32_t id = g_cpu.R[0];
    const uint32_t dst = g_cpu.R[1];
    const uint32_t cap = g_cpu.R[2];

    /* THE PAUSE MENU'S HIDDEN ROWS, AND IT MUST COME BEFORE THE CACHE.
     *
     * The pause menu's redraw asks for each row's text from one call site, so
     * whenever lr is that site the guest is inside the builder with the menu
     * object live in r10 - which is both the address the fixups need and proof
     * that the object exists. Poking it from the frame loop instead would mean
     * holding a pointer across the frames in which the menu is closed and the
     * object freed.
     *
     * Above the cache lookup because the cache returns early, and the six row
     * strings are cached after the menu's first frame - so a fixup below it
     * would run once and then never again. That is the same shape of bug as
     * the forced flag that was computed and dropped: correct code placed where
     * it does not execute. */
    /* TWO FRAMES UP - see kStringWrapperReturn. Checking our own caller was
     * never going to work: our caller is the wrapper, every time. */
    if (ui_fork_build() && g_cpu.R[14] == kStringWrapperReturn &&
        bus_device_read32(9, g_cpu.R[13] + kWrapperSavedLr) == kPauseRowReturn)
        pause_menu_fixups();

    /* THE CACHE, CHECKED BEFORE ANYTHING TOUCHES THE BUS. */
    if (id < kStringCacheIds && g_string_cache[id]) {
        const char* text = g_string_cache[id];
        uint32_t n = 0;
        while (text[n] && n + 1 < cap) ++n;
        const bool truncated = text[n] != '\0';

        /* THE TERMINATOR IS WRITTEN EXPLICITLY, and it has to be.
         *
         * A first version handed `n + 1` bytes straight to the writer, which is
         * right when `text[n]` is the terminator and WRONG when the caller's
         * buffer ran out first - then byte `n` is a real character and the
         * guest gets an unterminated string. The miss path below never had this
         * bug because it forces a terminator into its own buffer before
         * copying; the hit path has no buffer of its own to force it into. */
        char tail[1024];
        const uint32_t room = n < sizeof tail - 1 ? n : uint32_t(sizeof tail) - 1;
        std::memcpy(tail, text, room);
        tail[room] = '\0';

        ++g_cache_hits;
        write_string_to_guest(dst, tail, room + 1);
        g_cpu.R[0] = truncated ? 2u : 0u;   /* 2 == the caller's buffer filled */
        g_cpu.R[15] = g_cpu.R[14];
        return;
    }

    char buf[1024];
    const uint32_t room = cap < sizeof buf ? cap : uint32_t(sizeof buf);
    const Lsw2StringResult r = lsw2_string_decode(
        kLsw2StringControlBlock, id, buf, room, guest_read8, nullptr);

    if (r == Lsw2StringResult::NotReady) {
        /* The bank is not loaded. Say so the way the guest does rather than
         * writing anything - a caller that gets 1 does not read the buffer. */
        g_cpu.R[0] = 1;
        g_cpu.R[15] = g_cpu.R[14];
        return;
    }

    /* WRITE IT BACK A HALFWORD AT A TIME, because the bus exposes no 8-bit
     * device write. The length is rounded up and the terminator is already in
     * the buffer, so the extra byte written is either the terminator or past
     * a string we just terminated - never live data the guest still needs. */
    uint32_t n = 0;
    while (n < room && buf[n] != '\0') ++n;
    buf[n < room ? n : room - 1] = '\0';
    const uint32_t total = (n < room ? n : room - 1) + 1;
    write_string_to_guest(dst, buf, total);

    /* KEEP IT, so the next eight hundred bus reads never happen. Only a clean
     * decode: a truncated result is a fact about the CALLER's buffer, not about
     * the string, and storing it would hand the short version to a later caller
     * with room for all of it. */
    ++g_cache_misses;
    if (r == Lsw2StringResult::Ok && id < kStringCacheIds && !g_string_cache[id]) {
        if (char* keep = static_cast<char*>(std::malloc(total))) {
            std::memcpy(keep, buf, total);
            g_string_cache[id] = keep;
        }
    }

    if (g_string_hits == 0) {
        std::fprintf(stderr,
                     "[hle] string decoder override REACHED (id 0x%X -> \"%.40s\")\n",
                     unsigned(id), buf);
    }
    ++g_string_hits;

    /* BEFORE the return-state fixup below, so the backtrace sees the frame the
     * guest is actually in rather than the one it is about to return to. */
    if (trace_wanted(buf) || trace_id_wanted(id)) report_guest_stack(id, buf);

    g_cpu.R[0] = (r == Lsw2StringResult::Truncated) ? 2u : 0u;
    g_cpu.R[15] = g_cpu.R[14];
}

/* The override table.
 *
 * `validation` is null: these addresses are in the immutable cartridge image,
 * not code copied into RAM, so there is nothing whose bytes could change under
 * the entry. The two RAM banks need it and this does not. */
/* SORTED BY ADDRESS, ASCENDING, AND THAT IS A REQUIREMENT.
 *
 * lookup_in() BINARY-SEARCHES the table, so an unsorted one silently fails to
 * find entries that are present. The first version listed WH_GetSystemState
 * before the flag test - 0x020AD310 before 0x02086204, descending - and NEITHER
 * override was ever reached while the registration line printed happily.
 *
 * Nothing at NdsDispatchEntry's declaration says the table must be sorted: the
 * generated banks are sorted because a generator emitted them in address order,
 * so the requirement never had to be stated. A hand-written table is the first
 * one that can get it wrong. */
/* TWO BANKS, BECAUSE THEY HAVE DIFFERENT EVIDENCE BEHIND THEM.
 *
 * The fork cannot work without the first two. Its menu relabels are string
 * overrides, which only exist if the decoder is ours; its roster mode is the
 * flag test answering differently. Both are verified against the game rather
 * than assumed: `test_strings_real.cpp` decodes all 1333 strings from a
 * rebuilt-RAM fixture with zero mismatches, and the flag test reproduces a
 * seven-instruction body that is quoted in full above.
 *
 * The wireless trio is different. It DISPLACES THE VENDORED WIRELESS MODEL, and
 * the fork's own session is not yet driving it end to end - so switching it on
 * by default would change wireless behaviour ahead of the work that makes that
 * change correct. It stays explicit until Phase C lands.
 *
 * Each bank must be sorted ascending on its own; the addresses are disjoint so
 * two banks and one bank behave identically for lookup. */
/* ------------------------------------------------------------------------- */
/* THE LEVEL-SCRIPT COMMAND DISPATCHER, and the first use of the tail-call
 * passthrough.
 *
 * WHY THIS SEAM AND NOT ANOTHER. Three attempts to make the arena lobby's
 * console open the fork's menu each treated a symptom - two waived a popup in a
 * seam the dialog never crosses, and the third blanked the refusal's TEXT while
 * leaving the refusal in place. Fixing the cause rather than the symptom is the
 * lesson of that third one.
 *
 * The decision itself lives here. `0x02055E98` is a 192-entry command
 * dispatcher:
 *
 *     02055E9C  LDR  r3, =0x020ED928        the table
 *     02055EA0  LDR  r3, [r3, +r0, LSL #2]  handler = table[index]
 *     02055EA4  CMP  r3, #0                 a null slot is a no-op
 *     02055EAC  MOV  r0, r1                 shuffle the two arguments down
 *     02055EB0  MOV  r1, r2
 *     02055EB4  BLX  r3
 *
 * Every command the console's script issues passes through it, including the
 * one that raises the refusal - so this is where the fork can see what the
 * console actually DOES rather than what it says.
 *
 * THE PASSTHROUGH IS A TAIL CALL, which was worked out earlier and nothing had
 * used until now. An override REPLACES, so a seam this broad would normally be
 * unusable - 192 handlers cannot be reproduced. But this function's whole body
 * is "look one up and call it", which is the tail call's clean shape: set R[15] to
 * the handler with R[14] still holding our caller's return, and the handler
 * returns past us to whoever called the dispatcher.
 *
 * The stack must be checked for a tail call, and it is safe here. The original pushes
 * {r3, lr} before the call and pops {r3, pc} after; we push nothing and the
 * handler takes its two arguments in registers, so it neither reads what the
 * original pushed nor unbalances anything.
 *
 * WHAT IT DOES TODAY: nothing but log, in the fork, in the arena lobby, once per
 * distinct command index. The console's own indices are not yet known and
 * guessing one would be the fourth symptom-level patch. When the log names them,
 * acting on the right index is a branch in this same function - at the point
 * the game decides, which is where the fix belongs. */
constexpr uint32_t kScriptCmdTable = 0x020ED928u;
constexpr uint32_t kScriptCmdCount = 192u;

/* Command 168 raises a dialog whose id is `arg[0] + 0x2AE` - see the
 * interception below for the disassembly this comes from. */
constexpr uint32_t kScriptCmdDialog = 168u;

/* THE OTHER ONE, AND THE ONE THAT WAS ACTUALLY REFUSING.
 *
 * "You must Save before doing that!" was still on screen with the 168 waiver
 * installed. The waiver was watching the wrong
 * command.
 *
 * There are TWO commands that build a message id the same way, and only one of
 * them was known:
 *
 *   cmd 168  0x02059710   id = args[0] + 0x2AE, shown by calling 0x020601AC
 *   cmd 169  0x02059760   id = args[0] + 0x2AE, POSTED as state message 0x1A
 *
 *     02059780  LDRB r2, [r1]          args[0]
 *     0205978C  ADD  r2, r2, #0xAE
 *     02059790  ADD  ip, r2, #0x200    the same id arithmetic as 168
 *     02059794  MOV  r2, #0x1A         opcode 26
 *     02059798  MOV  r3, #1            one argument
 *     020597A0  BL   0x02060158        enqueue it
 *
 * 169 is the one the console uses, which is why every search that assumed 168
 * came back empty and why the 168 waiver never matched. It is also gated on
 * `[[0x02102900] + 4] == 4` - it only posts while the game is in state 4 - so
 * it is narrower than 168 rather than a duplicate of it.
 *
 * Both are waived, because both compose the same two refusal ids and there is
 * no reason to leave the second live once the first is closed. */
constexpr uint32_t kScriptCmdPostDialog = 169u;
constexpr uint32_t kScriptDialogBase = 0x2AEu;


void script_command() {
    /* THE GEAR HOP, OFFERED TO EVERY HOOK THAT RELIABLY FIRES.
     *
     * WHY MORE THAN ONE. 0x0202C218 was measured at sixty calls a second and
     * hosted the hop alone - and then in a later build it stopped being
     * dispatched at all, with the hop silently inert and customization after a
     * Free Play switch quietly broken again. The cause of that is not yet
     * understood, which is exactly why the hop must not depend on any single host.
     *
     * THIS IS SAFE BY CONSTRUCTION: the hop records WHICH host armed it and
     * only that host may complete it, so offering it to several costs nothing
     * and the first one to fire drains the queue.
     *
     * These three are dispatched in EVERY run this project has logged. They are
     * rarer than once a frame, which was why they were rejected as hosts for a
     * per-frame need - but applying a customization is not a per-frame need. A
     * hat that arrives a second late is invisible; a hat that never arrives is
     * the bug. */
    if (lsw2_gearhop_hook(0x02055E98u)) return;

    lsw2_probe_hook_rate("script_command");
    const uint32_t index = g_cpu.R[0];
    const uint32_t a = g_cpu.R[1];
    const uint32_t b = g_cpu.R[2];

    /* EVERY LEVEL, ONCE PER (command, level) - widened for a cantina/arena
     * comparison.
     *
     * This was gated to kLsw2ArenaLobbyLevel, which made it a cantina-only log.
     * The question it is now asked is a COMPARISON: the cantina's level script
     * ends up with thirteen zone-list nodes, eleven of a type the player can
     * attach to, and the arena's with one of a type it cannot. The
     * player is made playable BY attaching to a zone, so "which command builds
     * a zone, and does the arena run it" is the whole remaining question - and
     * a log that only ever sees one of the two levels cannot answer it.
     *
     * Keyed by level rather than latched once, so a command already reported in
     * the cantina still reports in the arena. Bounded at one line per
     * (command, level) - the cantina alone produced three. */
    if (ui_fork_build() && index < kScriptCmdCount) {
        static int seen_level[kScriptCmdCount];
        static bool seen_init = false;
        if (!seen_init) {
            seen_init = true;
            for (int i = 0; i < kScriptCmdCount; ++i) seen_level[i] = -1;
        }
        const int level = lsw2_cheats_current_level();
        if (seen_level[index] != level) {
            seen_level[index] = level;
            std::fprintf(stderr,
                         "[fork] script command %u on level %d "
                         "(args %08X %08X)\n",
                         unsigned(index), level, unsigned(a), unsigned(b));
        }
    }

    /* THE LOBBY CONSOLE (the fork's online menus), INTERCEPTED AT THE COMMAND
     * THAT REFUSES.
     *
     * Command 168 - handler 0x02059710 - is the level script's "raise a dialog",
     * and it composes the id from its own argument:
     *
     *     02059728  LDRB r2, [r4, #0]      one byte of the command's arguments
     *     02059734  ADD  r2, r2, #0xAE
     *     02059738  ADD  r2, r2, #0x200    id = arg[0] + 0x2AE
     *     0205973C  BL   0x020601AC
     *
     * So the console's two refusals are this command with argument 0x29
     * (0x2D7, "You must Save before doing that!") and 0x27 (0x2D5, "You can't do
     * that during a Wireless game!").
     *
     * REFUSING THE COMMAND, NOT THE MESSAGE. The previous attempt blanked the
     * decoded string, which left the refusal standing and put an empty dialog on
     * screen - a patch over the symptom. Declining to
     * dispatch the command means the dialog is never raised at all, which is the
     * condition rather than the symptom.
     *
     * THE ARGUMENT POINTER IS OUR `b`. The dispatcher shuffles r1->r0 and
     * r2->r1 before calling, so the handler's argument pointer is the value we
     * were given in r2. */
    if (ui_fork_build() &&
        (index == kScriptCmdDialog || index == kScriptCmdPostDialog) &&
        /* LOBBY STATE ONLY, not just the Cantina level: char-select (41)
         * and the rules chooser (54) share this level, and opening the console there
         * let the host reach the server-settings menu mid-setup, which is not
         * intended. Gate the console entity to the arena-lobby STATE (48) - the
         * gate all three hooks now share (console_open_allowed). Outside it the
         * command runs and the queue hook below declines what it posts. */
        console_open_allowed()) {
        const uint32_t msg = uint32_t(read_guest_byte(b)) + kScriptDialogBase;
        if (msg == kMsgMustSave || msg == kMsgWirelessBusy) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr,
                             "[fork] console refusal 0x%X declined at command "
                             "%u - opening the fork's menu\n",
                             unsigned(msg), unsigned(index));
            }
            lsw2_fork_console_interact();
            g_cpu.R[15] = g_cpu.R[14];       /* the command never runs */
            return;
        }
    }

    if (index >= kScriptCmdCount) {          /* out of the table entirely */
        g_cpu.R[15] = g_cpu.R[14];
        return;
    }

    const uint32_t handler =
        bus_device_read32(9, kScriptCmdTable + index * 4u);
    if (!handler) {                          /* the game's own null check */
        g_cpu.R[15] = g_cpu.R[14];
        return;
    }

    /* The argument shuffle the original does, then the tail call. R[14] is left
     * alone on purpose: that is what makes the handler return to our caller. */
    g_cpu.R[0] = a;
    g_cpu.R[1] = b;
    g_cpu.R[15] = handler;
}

/* THE LOBBY CONSOLE'S SAVE GATE, DECLINED WHERE THE DECLINING CAN ACTUALLY HAPPEN.
 *
 * WHY THE THREE EARLIER ATTEMPTS COULD NOT WORK, which is the whole finding.
 *
 * The refusal is NOT a level-script command. `script_command` above waives
 * command 168 in the arena lobby and that waiver is correct - but decoding the
 * arena lobby's own script (level 9's asset is **4264**, not 953; the script id
 * is field 5 of the 28-byte level record at `0x020F55C4`, and `tools/
 * analyze_level_scripts.py` had 953, which is level 1's) shows eighteen command
 * batches and **no command 168 anywhere in the level**. The console cannot be
 * refusing through a command the level never runs.
 *
 * It refuses through the STATE MESSAGE QUEUE instead:
 *
 *     0x0205FFB8   the queue pump; each entry is [opcode:16][argc:16] + args
 *       -> BLX vtable[5] of the current state object
 *     0x0208CFEC   this handler, opcodes 0..0x1A
 *       -> case 26  reads args[0] as a string id
 *     0x0208B3F0   raises the popup
 *
 * AND THE TWO ENQUEUE OVERRIDES ARE STRUCTURALLY DEAD. `0x02060084` and
 * `0x020600F0` are byte-identical twins, both registered above, and neither can
 * ever fire: the recompiler emits a **direct C call** for a branch whose target
 * lands in the same generated bank, and only cross-bank or indirect transfers
 * go through `runtime_dispatch` where an override lives. Both enqueues sit in
 * `lsw2_arm9_17.c` together with their callers, so the generated code calls
 * `lsw2_arm9_FUN_02060084()` by symbol and the dispatch table is never
 * consulted. That is exactly the reported symptom - "has never fired, in either
 * of its two forms" - and it was a property of the build, not of the test.
 *
 * WHY THIS SEAM IS DIFFERENT. `lsw2_arm9_FUN_0208cfec` has **zero direct call
 * sites** in the generated code: the pump reaches it through a vtable, so every
 * call is a `runtime_dispatch`, and an override here is reached. It is also in
 * bank 26 while the pump is in bank 17, so it would be dispatched even if the
 * call were direct.
 *
 * SCOPE, deliberately narrow. Opcode 26 carries every in-level hint in the game
 * - the whole 0x2B7-0x2D1 block - so only the two refusal ids are declined, and
 * only in the arena lobby, and only in the fork. Everything else, including
 * every other opcode, runs the cartridge's own code by calling the lifted
 * function directly. That is a real delegation and not a reimplementation: the
 * generated symbol is called the same way `guestcall.h` calls the gear applier,
 * which does not re-enter the dispatch table and so cannot recurse. */
void state_message() {
    /* THE GEAR HOP, OFFERED TO EVERY HOOK THAT RELIABLY FIRES.
     *
     * WHY MORE THAN ONE. 0x0202C218 was measured at sixty calls a second and
     * hosted the hop alone - and then in a later build it stopped being
     * dispatched at all, with the hop silently inert and customization after a
     * Free Play switch quietly broken again. The cause of that is not yet
     * understood, which is exactly why the hop must not depend on any single host.
     *
     * THIS IS SAFE BY CONSTRUCTION: the hop records WHICH host armed it and
     * only that host may complete it, so offering it to several costs nothing
     * and the first one to fire drains the queue.
     *
     * These three are dispatched in EVERY run this project has logged. They are
     * rarer than once a frame, which was why they were rejected as hosts for a
     * per-frame need - but applying a customization is not a per-frame need. A
     * hat that arrives a second late is invisible; a hat that never arrives is
     * the bug. */
    if (lsw2_gearhop_hook(0x0208CFECu)) return;

    /* SAY THAT IT RAN, ONCE.
     *
     * THE QUESTION THIS ANSWERS is not about this function. 0x0208CFEC is slot
     * +0x14 of the GAMEPLAY state's vtable at 0x020FF478, so if this override
     * fires then a state object's virtual calls do reach the dispatch table -
     * and three attempts to hook state 54's methods, all registered and all
     * silent, are about state 54 rather than about virtual dispatch. If it
     * never fires, they are about virtual dispatch and every plan that hooks a
     * state method is void.
     *
     * The override's own messages below are on the console-refusal path only,
     * so its silence has never distinguished "did not run" from "ran and had
     * nothing to say". */
    {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr,
                         "[hle] state_message REACHED - a state object's "
                         "vtable slot +0x14 dispatches\n");
        }
    }

    /* HOW OFTEN, NOT JUST WHETHER - off unless LSW2_PROBE_STATEMSG=1.
     *
     * WHAT THIS IS FOR. Re-applying customization after a Free Play switch, and
     * rendering the Jango/Boba head under customize headwear, both need the fork
     * to run the gear applier, and it cannot: called through the host seam it SPINS at
     * 0x020CEA8C, a wait-for-interrupt helper, because that seam advances only
     * the ARM9 between frames so the interrupt it waits for never arrives.
     * Reproduced on demand - seed a hat and the boot stalls with
     * "020063D4 did not return ... SPINNING AT ONE PC".
     *
     * The cure is the TRAMPOLINE HOP that already runs the cartridge's session
     * end on the guest's own thread, where both cores are
     * live and interrupts do fire. A hop has to be taken from inside a DISPATCH,
     * and the two hooks proven so far - the receive pump and the netgame tick -
     * both need a wireless session, which a solo Free Play character switch does
     * not have.
     *
     * This function is the candidate: 0x0208CFEC is slot +0x14 of the gameplay
     * state's vtable, and whsession.h's reading of the pump says the machine
     * calls that slot TWICE A FRAME. STRUCTURALLY per-frame, therefore - and
     * trusting structure over measurement has now been wrong twice, so it gets
     * counted before anything is built on it.
     *
     * The line above only ever proved it fires ONCE; a "said" flag cannot tell a
     * per-frame hook from a one-shot. */
    lsw2_probe_hook_rate("state_message");

    const uint32_t type = g_cpu.R[1];
    const uint32_t args = g_cpu.R[2];

    /* THE TOP-SCREEN PORTRAIT - IS IT EVER ASKED TO CHANGE?
     *
     * This override already stands in the one place that can answer the
     * portrait defect's last open question. The top-screen character icon is a HUD sprite whose
     * face texture is chosen when the portrait is BUILT, and the game's own way
     * of rebuilding it is message 0x15 to this very handler. So: if that
     * message arrives only while the level loads and never again, then a Free
     * Play character switch never asks for a new portrait, and that - not a
     * missed redraw, not a stale pointer, not the party refresh - is the whole
     * defect.
     *
     * NOTHING IS CHANGED HERE. This reports what the game already does, and
     * the numbers decide what gets built. Reporting the state pointer matters
     * as much as the character: a fix has to hand FUN_0208c024 the same object
     * the game hands it, and this is where that value is visible. */
    /* WHICH MESSAGES ARRIVE HERE AT ALL - the check that stops the block below
     * from being read as a measurement it is not.
     *
     * The first run of this reported ZERO portrait messages, including during
     * the level load, and a zero from an instrument nobody has validated says
     * nothing. So every message id is named the FIRST time it is seen. If 0x15
     * never appears in that list, the portrait really is never asked for; if
     * the list itself is short or empty, the override is not seeing the traffic
     * and no conclusion about 0x15 can be drawn from it. */
    {
        static bool seen_type[64] = {false};
        static long calls = 0;
        ++calls;
        if (type < 64u && !seen_type[type]) {
            seen_type[type] = true;
            std::fprintf(stderr,
                         "[fork] state_message: id 0x%02X first seen on call "
                         "%ld (state %08X)\n",
                         unsigned(type), calls, unsigned(g_cpu.R[0]));
        }
    }

    if (type == kStateMsgSetPortrait) {
        static int seen = 0;
        if (seen < 12) {
            ++seen;
            uint32_t a0 = 0, a1 = 0, a2 = 0;
            const bool readable = args >= 0x02000000u && args < 0x02400000u;
            if (readable) {
                a0 = bus_device_read32(9, args);
                a1 = bus_device_read32(9, args + 4u);
                a2 = bus_device_read32(9, args + 8u);
            }
            std::fprintf(stderr,
                         "[fork] portrait message: state %08X character %u  "
                         "args at %08X%s = %08X %08X %08X\n",
                         unsigned(g_cpu.R[0]), unsigned(a0 & 0xFFu),
                         unsigned(args), readable ? "" : " (UNREADABLE)",
                         unsigned(a0), unsigned(a1), unsigned(a2));
        }
    }

    if (ui_fork_build() && type == kStateMsgPopup &&
        args >= 0x02000000u && args < 0x02400000u &&
        lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel) {
        const uint32_t msg = bus_device_read32(9, args);
        if (msg == kMsgMustSave || msg == kMsgWirelessBusy) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr,
                             "[fork] console refusal 0x%X declined at the state "
                             "message - %s\n", unsigned(msg),
                             console_open_allowed()
                                 ? "opening the fork's menu"
                                 : "outside the lobby state, so nothing opens");
            }
            /* THE FORK OWNS THIS CONSOLE, so its refusal is declined on this
             * level whatever the state; the menu opens only where the console
             * may (console_open_allowed). */
            if (console_open_allowed()) lsw2_fork_console_interact();
            /* HANDLED, which is what stops the pump re-posting it. A zero
             * return sends the message to 0x0205FE58, the unhandled fallback,
             * which queues it again somewhere else - so returning 1 is part of
             * declining it and not a detail. */
            g_cpu.R[0] = 1u;
            g_cpu.R[15] = g_cpu.R[14];
            return;
        }
    }

    /* THE GAME'S OWN HANDLER, unchanged. R[15] has to name the entry because
     * the lifted function uses it to pick a landing pad, and R[14] is left
     * alone so it returns straight to the pump. */
    g_cpu.R[15] = kLsw2LevelStateMessage;
    lsw2_arm9_FUN_0208cfec();
}

/* THE ENQUEUE, AT AN ADDRESS THAT IS ACTUALLY REACHED.
 *
 * `script_enqueue` above was registered on 0x02060084 and 0x020600F0 and could
 * never run. Both are in `lsw2_arm9_17.c` together with their only callers, and
 * the recompiler emits a DIRECT C call - `lsw2_arm9_FUN_02060084()` - whenever a
 * branch target lands in the same generated bank. Only cross-bank and indirect
 * transfers go through `runtime_dispatch`, which is the only place an override
 * lives. `tools/sweep_override_reachable.py` now measures exactly this.
 *
 * 0x02060158 is the varargs wrapper in front of them:
 *
 *     02060158  PUSH {r0, r1, r2, r3}      spill, so the varargs can be walked
 *     02060170  LDR  r3, [sp, #0x14]       argc
 *     02060174  BL   0x02060084            the real enqueue
 *
 * and it has **zero direct call sites** in the generated code, so every call to
 * it dispatches. Script command 169 - the one that raises the console's refusal
 * - reaches the queue through here, which is what makes this the right address
 * rather than merely a reachable one.
 *
 * IT ONLY WATCHES. The fork needs an object and a player index to raise a popup
 * of its own and cannot look either up; every enqueue passes through here, so
 * observing is enough. Everything else is the cartridge's own code, called by
 * symbol - which does not re-enter the dispatch table and so cannot recurse. */
void script_enqueue_wrapper() {
    const uint32_t obj = g_cpu.R[0];
    const uint32_t index = g_cpu.R[1];
    const uint32_t opcode = g_cpu.R[2];
    const uint32_t argc = g_cpu.R[3];

    /* THE VARARGS START AT THE CALLER'S SP. 0x02060158 is `PUSH {r0-r3}`
     * followed by the real enqueue, so its fifth parameter onwards are stack
     * arguments; our override runs with no prologue executed, so g_cpu.R[13] is
     * still the caller's sp and the first of them sits exactly there. Command
     * 169 is the proof - `STR ip, [sp]` immediately before `BL 0x2060158`. */
    const uint32_t arg0 =
        (argc >= 1u) ? bus_device_read32(9, g_cpu.R[13]) : 0u;

    if (g_enqueue_hits == 0) {
        std::fprintf(stderr,
                     "[fork] enqueue seam REACHED (opcode %u, %u arg(s))\n",
                     unsigned(opcode), unsigned(argc));
    }
    ++g_enqueue_hits;

    /* WHICH OPCODE IS WHICH is not settleable by reading, because it arrives in
     * a register from script data. Turn this on, do the one thing you want to
     * identify, read the opcode back:
     *
     *     LSW2_TRACE_ENQUEUE=1 ./lsw2_bountyhunter
     *
     * Off by default: this fires for every scripted command in the game. */
    static const bool trace = [] {
        const char* v = std::getenv("LSW2_TRACE_ENQUEUE");
        return v && *v == '1';
    }();
    if (trace) {
        std::fprintf(stderr, "[trace] enqueue op %u argc %u arg0 %08X "
                             "(obj %08X idx %u)\n",
                     unsigned(opcode), unsigned(argc), unsigned(arg0),
                     unsigned(obj), unsigned(index));
    }

    /* THE SAVE GATE, AT THE QUEUE - and this is a LIVE test rather than the
     * retired one it replaces.
     *
     * The version that sat on 0x02060084 was narrowed with the
     * note "the trace settled it: the dialog is raised through 0x0206C8D4 and
     * never crosses the script enqueue at all". **That conclusion cannot
     * stand.** The override it was drawn from was registered on an address the
     * recompiler never dispatches to, so it produced no observations at all;
     * "the trace showed nothing here" was a property of the seam being dead,
     * not of the traffic through it.
     *
     * The refusal does come through here. Script command 169 (0x02059760)
     * composes `args[0] + 0x2AE` and posts it as opcode 26 through this exact
     * function. This is the second of two places the fork declines it - the
     * first is at the command, one step earlier and therefore preferred; this
     * catches anything that reaches the queue by another route. */
    /* ON THE LOBBY LEVEL ONLY, which is where the fork's console is: the
     * refusal is the console script's, so it is declined there in every state,
     * and the menu opens only where the console may (console_open_allowed).
     * This hook had no gate at all, and it is the one the statue select's A
     * reached, opening the console during character select. */
    if (ui_fork_build() && opcode == kLsw2ScriptOpShowMessage && argc >= 1u &&
        (arg0 == kMsgMustSave || arg0 == kMsgWirelessBusy) &&
        lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel) {
        const bool open = console_open_allowed();
        static int said = 0;
        if (said < 8) {
            ++said;
            std::fprintf(stderr,
                         "[fork] console refusal 0x%X declined at the queue - "
                         "%s (screen state %d)\n", unsigned(arg0),
                         open ? "opening the fork's menu"
                              : "outside the lobby state, so nothing opens",
                         lsw2_state_current());
        }
        if (open) lsw2_fork_console_interact();
        g_cpu.R[15] = g_cpu.R[14];   /* never queued */
        return;
    }

    /* THE GAME'S OWN ENQUEUE, called by symbol - which does not re-enter the
     * dispatch table and so cannot recurse. */
    g_cpu.R[15] = kScriptEnqueueWrapper;
    lsw2_arm9_FUN_02060158();
}

/* A PROBE, AND DELIBERATELY NOTHING ELSE YET.
 *
 * The whole point of this function today is to answer one question that has
 * never been answered by measurement: DOES AN OVERRIDE FIRE ON THE GUEST'S
 * THREAD WHILE THE PLAYER STANDS IN A SETTLED LOBBY. An earlier reading said no,
 * but that was read off the coverage manifest, and the manifest is
 * not an execution trace - it records entry into INTERPRETED code, so a lifted
 * function called through dispatch never appears in it. So the claim is
 * unproven rather than false, and this settles it either way.
 *
 * IT MUST CALL THE PUMP ITSELF. The dispatch table is REPLACEMENT-ONLY: an
 * override does not wrap the function, it stands in for it. Returning without
 * calling the pump would stop the cartridge consuming its own received
 * messages - which is the path the round start arrives on - so the
 * chain below is not optional politeness, it is the whole function still
 * working.
 *
 * ONCE A SECOND, NOT EVERY FRAME. A per-frame line would bury every other
 * diagnostic in the log, and the question is "does it fire at all, and does it
 * keep firing after the lobby settles", which one line a second answers. */
/* FORCE 41 -> 42 IN-FRAME. The guest is never guest-call-ready at
 * state 41 (a busy-wait), so a between-frames guest-call cannot drive it. This
 * hook runs ON the guest thread each frame, so it can call the dispatcher the
 * guest itself would. Once settled at 41, synthesize the message the arena
 * start is: msg type 0x0D, which the dispatcher's case '\r' turns into a request
 * for state 0x2a (42). case '\r' reads only the type at msg+1 and uses globals
 * for the rest, so an unused station-3 ring slot serves as the message scratch.
 * Behind LSW2_FORCE_MATCH; off by default. `netgame` is the pump's own argument.
 * Clobbers g_cpu, so the caller restores R[0] before the real pump. */
static void pump_force_match(uint32_t netgame) {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_FORCE_MATCH");
        return e && *e == '1';
    }();
    if (!on) return;
    if (netgame < 0x02000000u || netgame >= 0x02400000u) return;

    static bool done = false;
    if (done) return;

    static int settle = 0;
    if (lsw2_state_current() != 41) { settle = 0; return; }
    if (++settle < 150) return;   /* ~6 s at ~24 fps: let both reach 41 */

    done = true;

    /* Station 3's message ring is unused in a 2-player match: safe scratch for
     * the synthetic message. Set the TYPE byte (msg + 1) to 0x0D via a word RMW
     * (the bus has no 8-bit write). msg is word-aligned, so msg+1 is byte 1. */
    const uint32_t msg = netgame + 0xa3cu + 3u * 0x344u;
    const uint32_t addr = msg + 1u;
    const uint32_t wbase = addr & ~3u;
    const uint32_t sh = (addr & 3u) * 8u;
    const uint32_t w = bus_device_read32(9, wbase);
    bus_device_write32(9, wbase, (w & ~(0xFFu << sh)) | (uint32_t(0x0Du) << sh));

    std::fprintf(stderr,
                 "[fork] pump force: settled at 41; dispatching a synthetic "
                 "0x0D in-frame (netgame %08X, msg %08X) to request state 42"
                 "\n", unsigned(netgame), unsigned(msg));

    g_cpu.R[0] = netgame;
    g_cpu.R[1] = msg;
    g_cpu.R[2] = 0;
    g_cpu.R[3] = 0;
    g_cpu.R[15] = 0x020B1248u;
    lsw2_arm9_FUN_020b1248();
}

/* THE NETGAME MESSAGE RELAY, SEND half. The cartridge's own netgame
 * message stream never crosses the DS-WM link (never established), so the fork
 * carries it. This runs at the receive-pump hook, in-frame on the guest thread,
 * and walks the LOCAL station's message ring - a linked list of 4-byte entries
 * at kLsw2MsgOwner + station*0x344 + 0xA3C: [+0] low byte = delta (4-byte units)
 * to the next entry (0 ends), [+1] = type, [+2..3] = seq. For each entry whose
 * seq it has not relayed, it sends the whole record (delta*4 bytes) to the peer,
 * which will dispatch it in-frame (RECEIVE half, next). Behind LSW2_MSG_RELAY. */
static void pump_relay_send() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_MSG_RELAY");
        return e && *e == '1';
    }();
    if (!on || !ui_fork_build()) return;

    const int mask = lsw2_viewmask_guest();
    if (mask <= 0) return;
    if (lsw2_viewmask_count(unsigned(mask), kLsw2ViewRecords) < 2) return;

    const int local = lsw2_fork_session().local_station();
    if (local < 0 || local >= kLsw2MsgStations) return;

    const uint32_t ring = kLsw2MsgOwner +
                          uint32_t(local) * kLsw2MsgStationStride +
                          kLsw2MsgSlotBase;

    static int last_seq[128];
    static bool init = false;
    if (!init) { for (int i = 0; i < 128; ++i) last_seq[i] = -1; init = true; }

    uint32_t slot = 0;
    for (int guard = 0; guard < 64; ++guard) {
        const uint32_t entry = ring + slot * 4u;
        const uint32_t raw = bus_device_read32(9, entry);
        const uint32_t delta = raw & 0xFFu;
        if (delta == 0) break;                    /* end of the list */
        const int type = int((raw >> 8) & 0xFFu);
        const int seq = int((raw >> 16) & 0xFFFFu);

        if (slot < 128 && type != 0 && last_seq[slot] != seq) {
            last_seq[slot] = seq;
            int len = int(delta * 4u);
            if (len > kLsw2ShareBytes) len = kLsw2ShareBytes;
            uint8_t rec[kLsw2ShareBytes];
            for (int i = 0; i < len; i += 4) {
                const uint32_t w = bus_device_read32(9, entry + uint32_t(i));
                rec[i + 0] = uint8_t(w);
                rec[i + 1] = uint8_t(w >> 8);
                rec[i + 2] = uint8_t(w >> 16);
                rec[i + 3] = uint8_t(w >> 24);
            }
            lsw2_fork_session().send_netmsg(local, rec, len);
            static int said = 0;
            if (said < 40) {
                ++said;
                std::fprintf(stderr,
                             "[fork] msg relay SEND: station %d slot %u type "
                             "0x%02X seq %d len %d\n",
                             local, unsigned(slot), unsigned(type), seq, len);
            }
        }
        slot += delta;
        if (slot >= 120u) break;                  /* ring bound */
    }
}

/* THE NETGAME MESSAGE RELAY, RECEIVE half - drain + log for now.
 * Verifies the SEND half's records cross the transport before the in-frame
 * dispatch (FUN_020b1248) is wired. Behind LSW2_MSG_RELAY. */
static void pump_relay_recv() {
    static const bool on = [] {
        const char* e = std::getenv("LSW2_MSG_RELAY");
        return e && *e == '1';
    }();
    if (!on || !ui_fork_build()) return;

    /* APPEND, ONE PER FRAME. Direct in-frame dispatch via
     * FUN_020b1248 was FLAKY (out-of-context call races the arena rebuild). So
     * instead APPEND the relayed record to the sender's message ring on this
     * console and let the game's OWN receive pump (which runs right after this,
     * in netmsg_pump_probe) dispatch it in full context and order. The ring is a
     * linked list of 4-byte entries at kLsw2MsgOwner + station*0x344 + 0xA3C:
     * walk to the delta=0 terminator, write the record there (its [+0] byte is
     * already the delta/length), and lay a new 0 terminator after it. One record
     * per frame, so the game consumes between appends. */
    int station = 0, len = 0;
    uint8_t rec[kLsw2ShareBytes];
    if (!lsw2_fork_session().take_netmsg(&station, rec, &len)) return;
    if (len < 4 || station < 0 || station >= kLsw2MsgStations) return;
    const int delta = rec[0] & 0xFF;
    if (delta <= 0) return;
    int rlen = delta * 4;
    if (rlen > len) rlen = len;
    if (rlen > kLsw2ShareBytes - 4) return;

    const uint32_t ring = kLsw2MsgOwner +
                          uint32_t(station) * kLsw2MsgStationStride +
                          kLsw2MsgSlotBase;

    uint32_t slot = 0;
    int guard = 0;
    while (guard++ < 64) {
        const uint32_t d = bus_device_read32(9, ring + slot * 4u) & 0xFFu;
        if (d == 0) break;
        slot += d;
        if (slot >= 100u) return;                 /* ring near full - skip */
    }

    const uint32_t at = ring + slot * 4u;
    for (int i = 0; i < rlen; i += 4) {
        const uint32_t w = uint32_t(rec[i]) |
                           (uint32_t(i + 1 < len ? rec[i + 1] : 0) << 8) |
                           (uint32_t(i + 2 < len ? rec[i + 2] : 0) << 16) |
                           (uint32_t(i + 3 < len ? rec[i + 3] : 0) << 24);
        bus_device_write32(9, at + uint32_t(i), w);
    }
    const uint32_t term = at + uint32_t(rlen);
    const uint32_t tw = bus_device_read32(9, term);
    bus_device_write32(9, term, tw & ~0xFFu);       /* delta 0 = end */

    const int type = rec[1];
    const int seq = rec[2] | (rec[3] << 8);
    static int said = 0;
    if (said < 60) {
        ++said;
        std::fprintf(stderr,
                     "[fork] msg relay APPEND: station %d type 0x%02X seq %d at "
                     "slot %u (game's pump dispatches it)\n",
                     station, unsigned(type & 0xFF), seq, unsigned(slot));
    }
}

void netmsg_pump_probe() {
    const uint32_t netgame = g_cpu.R[0];   /* the pump's argument */

    /* THE SAMPLER HAS RUN (it is first in the manager's frame) and no state
     * has updated yet: a paused station's input goes before anything reads
     * it (sessionpause.h). */
    lsw2_sessionpause_after_sampler();

    static int beat = 0;
    static int said = 0;
    if (++beat >= 60) {
        beat = 0;
        if (said < 30) {
            ++said;
            std::fprintf(stderr,
                         "[hle] pump probe: the receive pump override IS "
                         "running on the guest thread\n");
        }
    }

    /* THE "SHUT DOWN LOBBY" SESSION END, taken from here because this is the
     * only hook measured to fire on the guest's thread while a peer is
     * connected. Returning immediately is the point: the guest resumes inside
     * the cartridge's own end and comes back to this same function. */
    if (lsw2_wh_session_end_hop(kNetMsgPump)) return;

    /* Force 41 -> 42 in-frame when armed (no-op unless LSW2_FORCE_MATCH). It may
     * clobber g_cpu by calling the dispatcher, so restore R[0] afterwards. */
    pump_force_match(netgame);

    /* Relay this console's netgame messages to the peer, and drain+log any the
     * peer sent us (no-op unless LSW2_MSG_RELAY). C++ only, no guest state. */
    pump_relay_send();
    pump_relay_recv();

    /* THE GAME'S OWN PUMP, unchanged. R[15] names the entry for the same
     * reason the state-message handler above does. */
    g_cpu.R[0] = netgame;
    g_cpu.R[15] = kNetMsgPump;
    lsw2_arm9_FUN_020b0410();
}

/* THE PAUSE MENU'S MESSAGES, ONE PLAYER'S AT A TIME.
 *
 * The cartridge's multi-card pause is one pause for everyone. A Start press
 * with the multi-view bit up sends message 0x2B (0x0208CEF8) instead of
 * pausing, and every console's handler (0x020B2770) freezes its world and opens
 * the pause menu. The Resume row sends 0x2C; the confirmed Quit row sends 0x42
 * (0x0208EF70), and every console's 0x42 arm (0x020B2E7C) ends the session:
 * the hosting console through 0x020B111C, the rest through the finalize
 * 0x02072BC8. Measured (conn_disc, four consoles): one client's Disconnect took
 * the host and both other clients out of the session at one dataset.
 *
 * The rules are sessionpause.h's: the sender's console opens its own menu with
 * nothing frozen, every other console drops the pause, the resume and a
 * client's quit, and every console keeps the same record of who is paused so
 * that player's character stands still everywhere. The host's quit - "Shut
 * Down Lobby" - still ends the session for everyone.
 *
 * RETRACTED: the first version of this override turned a
 * client's 0x42 into 0x2C on the other consoles, to close the pause menu the
 * shared 0x2B had opened on them. With the pause no longer shared nobody else
 * has a menu to close, and a 0x2C with no menu up would ask for whatever state
 * manager+0x28 last recorded - so the others now drop it instead.
 *
 * WHY HERE. 0x020B1248 has one caller, the pump, in another shard, so it is
 * reached through the dispatcher and this fires for every message (0 direct
 * calls in the generated banks). The sender's row action (0x0208EA58) and
 * send (0x020B04F0) are called directly and can never be overridden. */
namespace {
constexpr uint32_t kNetMsgDispatch = 0x020B1248u;
}  // namespace

void netmsg_dispatch_override() {
    const uint32_t msg = g_cpu.R[1];
    const int station = int(g_cpu.R[2] & 0xFFu);
    if (msg >= 0x02000000u && msg < 0x02400000u &&
        lsw2_sessionpause_message(msg, station)) {
        /* HANDLED HERE: dropped, or this console's own menu requested. The
         * dispatcher returns nothing its caller reads. */
        g_cpu.R[15] = g_cpu.R[14];
        return;
    }
    /* THE GAME'S OWN DISPATCH, as the override's last act. */
    g_cpu.R[15] = kNetMsgDispatch;
    lsw2_arm9_FUN_020b1248();
}


/* THE PAUSE STATE'S UPDATE (vtable 0x020FF524 slot 3), reached only through
 * that vtable: this console's own input word back for the menu's own call
 * (sessionpause.h), and the host of the pause rebuild's hop (pauseredraw.h). */

void pause_state_update_override() {
    /* THE PAUSE REBUILD'S HOST (pauseredraw.h): a queued rebuild runs here,
     * on the guest's thread, before this update reads the pad. When the hop
     * redirects the guest this override must return at once; it runs its own
     * body when the rebuild comes back. */
    if (lsw2_gearhop_hook(kLsw2PauseStateUpdate)) return;
    lsw2_sessionpause_menu_update();
    g_cpu.R[15] = kLsw2PauseStateUpdate;
    lsw2_arm9_FUN_0208e514();
}

/* 0x020B8E40, called by the manager's frame (0x0205ED08) between the state
 * updates and the entity pass, in another shard: a paused station's input is
 * taken away again before any entity reads it (sessionpause.h). */
constexpr uint32_t kBetweenPasses = 0x020B8E40u;

void between_passes_override() {
    lsw2_sessionpause_before_entities();
    g_cpu.R[15] = kBetweenPasses;
    lsw2_arm9_FUN_020b8e40();
}

/* A PROBE, exactly like the pump's was, and for the same reason.
 *
 * The pump probe taught this the expensive way: it printed NOTHING in a
 * solo lobby and that looked like a refutation, when the pump is simply never
 * called with one player. So this proves it fires - and, because the whole point
 * is the moment a peer leaves, it reports the player count alongside, so a later
 * run can see whether it KEEPS firing after that count drops to 1.
 *
 * IT MUST CALL THE TICK ITSELF. The table is REPLACEMENT-ONLY. This function
 * walks the station packet areas and drives the whole netgame frame; returning
 * without it would stop far more than the error. */
void netgame_tick_probe() {
    /* EVERY FRAME, not once a second - the watch below acts on an EDGE, and a
     * sampled count would miss the frame the peer left on. Only the LOG is
     * rationed. */
    const uint32_t owner = 0x021027F8u;
    const uint32_t w = bus_device_read32(0, (owner + 0x29u) & ~3u);
    const unsigned count = (w >> (((owner + 0x29u) & 3u) * 8u)) & 0xFFu;

    static int beat = 0;
    static int said = 0;
    if (++beat >= 60) {
        beat = 0;
        if (said < 40) {
            ++said;
            std::fprintf(stderr,
                         "[hle] netgame tick probe: running on the guest "
                         "thread, player count %u | data-share %llu steps "
                         "%llu stalls (a joiner that never calls the "
                         "step produces no block, so the host's step stalls)\n",
                         count,
                         static_cast<unsigned long long>(g_step_hits),
                         static_cast<unsigned long long>(g_step_stalls));
        }
    }

    /* NET38 DIAG (LSW2_NET38_DIAG): the tick-tolerate guard fired 0x during a 3-station
     * join, so the child2 38 is NOT the guarded netgame-tick site (bl 0x020b04f0 @
     * 0x020B08EC). Log the consistency error word 0x021092FC (low byte: 0=OK,
     * 1=view-count-0, 2=checksum-mismatch) whenever it CHANGES, with the count/WH, to
     * identify the real 38 source for the 2nd child. Host + fork only, log-only, off by
     * default so 2-player is untouched. */
    {
        static const bool net38_diag = [] {
            const char* e = std::getenv("LSW2_NET38_DIAG");
            return e && *e == '1';
        }();
        if (net38_diag && ui_fork_build() && lsw2_wh_role_is_host()) {
            const uint32_t err = bus_device_read32(0, 0x021092FCu);
            static uint32_t last_err = 0xFFFFFFFFu;
            if (err != last_err) {
                last_err = err;
                std::fprintf(stderr,
                             "[fork] net38-diag: 0x021092FC = 0x%08X (low=%u) "
                             "count=%u wh=%d beat=%d [low: 0=OK 1=view0 2=checksum]\n",
                             err, err & 0xFFu, count, lsw2_wh_state(), beat);
            }
        }
    }

    /* THE JOINER'S HALF OF "SHUT DOWN LOBBY", AND THE COMMUNICATION ERROR THAT
     * FOLLOWS A HOST LEAVING. This hook, and not the pump, because this one
     * was measured still firing at a player count of ONE - the
     * pump's guard is `1 < count` and it stops exactly when the joiner needs
     * it. The watch decides; it does nothing on a host. */
    lsw2_wh_joiner_watch(count);

    /* FOUR PLAYERS: re-open the host's WM entry from HERE - within
     * guest execution, scheduler live - so a 2nd/3rd/4th child can associate. Doing
     * it from the fork tick (between frames) dropped the first child.
     * Host-only, gated behind LSW2_KEEP_ENTRY; a no-op on a joiner. */
    lsw2_wh_keep_entry_tick();

    /* AND THE HOP IT ARMS, taken here for the same reason. The hop knows which
     * hook armed it, so this and the pump cannot complete each other's return
     * (see g_end_host in whsession.cpp). */
    if (lsw2_wh_session_end_hop(kLsw2NetGameTick)) return;

    /* THE GAME'S OWN TICK, unchanged. */
    g_cpu.R[15] = kLsw2NetGameTick;
    lsw2_arm9_FUN_020b07e8();

    /* FOUR PLAYERS: advance the net38 tolerance streak. The lifted
     * tick above calls lsw2_fork_suppress_net38() at its state-38 request (post-lift
     * guard); this tail decides whether the count-0 was transient (a join settling,
     * keep suppressing) or persistent (a real empty session, let 38 fire). */
    lsw2_wh_net38_frame_end();
}

/* THE GAMEPLAY STATE'S PER-FRAME UPDATE, counted before anything is built on it.
 *
 * MUST CALL THE STATE'S OWN UPDATE. The table is REPLACEMENT-ONLY, and this is
 * the function that advances the whole gameplay state every frame - returning
 * without it would stop the game, not just the measurement. */
void freeplay_message_probe() {
    lsw2_probe_hook_rate("freeplay_message");

    g_cpu.R[15] = kFreePlayMessage;
    lsw2_arm9_FUN_02088014();
}

/* THE PER-FRAME HOOK THAT HOSTS THE GEAR HOP - customization after a Free Play
 * switch, and the Jango/Boba head under customize headwear.
 *
 * 0x0202C218 was MEASURED at sixty calls per second in a solo lobby with no
 * wireless session, which is what four iterations of reading the
 * game's code failed to find. Two more hooks with the same property are known,
 * so this choice is not load-bearing.
 *
 * THE HOP FIRST, then the function's own body. When the hop returns true it has
 * pointed the guest at the cartridge's applier and this override must return at
 * once - it is not being asked to do its own job on that pass. */
void frame_cand2_probe() {
    lsw2_probe_hook_rate("frame_cand2_0204a620");
    g_cpu.R[15] = 0x0204A620u;
    lsw2_arm9_FUN_0204a620();
}

void frame_cand3_probe() {
    lsw2_probe_hook_rate("frame_cand3_02055530");
    g_cpu.R[15] = 0x02055530u;
    lsw2_arm9_FUN_02055530();
}

void frame_cand1_probe() {
    lsw2_probe_hook_rate("frame_cand1_0202c218");

    if (lsw2_gearhop_hook(kFrameCand1)) return;

    g_cpu.R[15] = kFrameCand1;
    lsw2_arm9_FUN_0202c218();
}

/* THE HUD'S OWN FRAME STEP, hosting the top-screen portrait's hop, after the
 * return of all players to the lobby.
 *
 * The goal, in plain English: sending every player back to the lobby has to
 * leave the host alive at the end. The return was measured working on both consoles
 * and the host then dying, twice, between the portrait's second sprite
 * destructor and its rebuild - the three steps that pass at boot. They were
 * being taken from the host above, inside an ACTOR's update, and the game
 * deletes those sprites from inside the HUD's update (FUN_0208b0b8, reached
 * from this function). Same steps, wrong phase: a deleting destructor run
 * where the cartridge never runs one.
 *
 * This host is asked for BY NAME (lsw2_gearhop_request_call_at with
 * kLsw2HudStep), so the gear applier and the party refresh keep their hosts and
 * only the portrait's three steps move. FUN_0208cdb0 is reached through the
 * vtable word at 0x020FF484 and nothing else - one DATA reference, zero BL -
 * so every call is dispatched and this fires; the caller-count gate records
 * it as one caller for that reason. The body is the cartridge's own, run after
 * the hop has had its turn, exactly as the host above does. */
void hud_step_hop_host() {
    /* THE VIEW-MANAGER HOP ALSO RIDES THIS PER-FRAME HOST. Its
     * other host is the Free Play widget's flag test (0x02086204), which fires
     * only while that menu is up - so a JOINER sitting in the plain arena lobby
     * never triggers it and its view manager never builds. This host runs every
     * frame in the lobby on the guest thread, so an armed hop is taken promptly
     * wherever the joiner is. Inert unless lsw2_wh_make_view_manager armed one. */
    if (lsw2_wh_trampoline_hop(kLsw2HudStep)) return;

    if (lsw2_gearhop_hook(kLsw2HudStep)) return;

    g_cpu.R[15] = kLsw2HudStep;
    lsw2_arm9_FUN_0208cdb0();
}

/* THE ARENA LOBBY KEEPS THE PLAYER: THE CANTINA EXIT DOOR DISABLED AT ITS OWN
 * TRIGGER.
 *
 * The exit is a trigger entity (vtable 0x020FF924); FUN_02097BCC is its contact
 * handler (vtable slot +0x10, zero direct BL callers - purely vtable-dispatched,
 * so this override fires on every contact). Decompiled, it is:
 *
 *     FUN_020977f8(entity, actor);              // bookkeeping, no fade
 *     if (*(char*)(entity + 0x3d) != 0) return; // the door's OWN disable flag
 *     FUN_02097bfc(entity+0x3b, ...);           // -> the warp (arms the fade,
 *                                               //    then requests state 30 +
 *                                               //    level 14)
 *
 * entity+0x3b is the door's target level (14 = Mos Eisley Cantina Main) and
 * entity+0x3d is the game's OWN "this door is disabled" flag, checked here and
 * written by the arm routine FUN_02097b98. So the clean disable is to set that
 * flag before delegating: the handler then returns early, UPSTREAM of everything
 * - no fade is armed (FUN_0206dd60 never runs), no state-30 or level-14 request
 * is posted, so there is nothing for pin_level to rewrite, no lobby reload, no
 * hanging fade, no crash.
 *
 * Earlier attempts failed for want of this seam: refusing the level request at
 * 0x0205ECC4 left the already-armed fade with nothing to complete (freeze, see
 * statereq.cpp), and an override on a different controller (0x02073F54) never
 * fired because that object is not instantiated in the lobby.
 *
 * Gated to the arena lobby (level 9) and to the exit-to-Cantina door
 * (entity+0x3b == 14), so no other door, level, or the baseline is touched. */
void lsw2_doorseal_contact() {
    const uint32_t entity = g_cpu.R[0];
    if (ui_fork_build() && entity >= 0x02000000u && entity < 0x02400000u &&
        lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel) {
        const uint32_t w3b = bus_device_read32(9, (entity + 0x3Bu) & ~3u);
        const uint8_t target =
            uint8_t((w3b >> (((entity + 0x3Bu) & 3u) * 8u)) & 0xFFu);
        if (target == 14u) {   /* the exit to Mos Eisley Cantina Main */
            const uint32_t base = (entity + 0x3Du) & ~3u;
            const uint32_t shift = ((entity + 0x3Du) & 3u) * 8u;
            const uint32_t w = bus_device_read32(9, base);
            if (((w >> shift) & 0xFFu) == 0u) {   /* only when not already disabled */
                bus_device_write32(9, base, (w & ~(0xFFu << shift)) | (1u << shift));
                static bool said = false;
                if (!said) {
                    said = true;
                    std::fprintf(stderr,
                                 "[fork] lobby door: set the cantina exit's own "
                                 "disable flag (entity %08X +0x3D=1) - the door "
                                 "no longer warps, before any fade.\n",
                                 unsigned(entity));
                }
            }
        }
    }
    g_cpu.R[15] = 0x02097BCCu;
    lsw2_arm9_FUN_02097bcc();
}

/* BUILD THE SOLO LOBBY PLAYER AS THE LAST-PLAYED HUNTER, DIRECTLY.
 *
 * The player appears in the arena lobby (the Mos Eisley Cantina) straight away, as
 * the Bounty Hunter they last played (Greedo if there is none). It is the
 * lobby's own actor-list entry that spawns the player: this KIND-0 builder (0x02050C44) reads
 * the character id from a byte at descriptor+8 and the party slot from
 * descriptor+9, then installs the actor (0x02091B88 -> ctor 0x0200D6F4 ->
 * 0x02001D20, which writes actor+0x6BB = id). The lobby data spawns slot 0 as
 * Greedo (20). Rewriting that one byte BEFORE the build reads it makes the actor
 * be CONSTRUCTED as the hunter - never Greedo, no visible switch.
 *
 * WHY NOT THE PENDING BYTE. lasthunter_observe.cpp's apply writes the pending
 * character (+0x92E) and the poller 0x02091604 swaps the built actor ~10 frames
 * later - a visible Greedo->hunter switch, and it CANNOT be pre-empted: the same
 * poller erases a request while the actor slot is still null. Worse,
 * the observe path then remembered the default Greedo over the saved hunter
 * before the swap took (measured: saved 29 came back 20, no apply
 * line). Building as the hunter removes both faults at once - the player is never
 * the default, so nothing overwrites the saved hunter, and there is no switch.
 *
 * THE SEAM. 0x02050C44 has zero direct BL callers (reached only through the
 * indirect BLX in dispatcher 0x02067838's KIND table 0x020ED5CC), so this
 * override fires; and its body ends in a tail call to 0x02091B88 with no
 * post-work, so patch-then-delegate is loss-free. This is
 * the seam an earlier search missed: it examined the ctor 0x0200D6F4 (post-call work) and
 * 0x02001D20 (mid-function) and rejected both, but not the builder that SUPPLIES
 * the id and has nothing after the install.
 *
 * SCOPE. Fork build; slot 0 only while nobody is seated (the multi-view bit
 * clear - see the branch below; it used to say OFFLINE only); once
 * seated, the seating/roster owns the
 * character and the online join-in-progress case stays with the pending-byte apply;
 * party slot 0 only (desc+9 == 0), so the lobby companion is untouched; arena
 * lobby only. The hunter id is the fork's validated bh_last_hunter, defaulting to
 * Greedo (20) - so nothing saved means the byte is rewritten to what it already
 * was, a no-op. desc+8 is a byte with three unrelated neighbours, hence the
 * byte-safe write_guest_byte. Customization composes for free: the builder reads
 * the customize staging slots at build time, which the customization
 * tick has already pushed, so the hunter is built wearing the saved hat/weapon. */
void lsw2_lobby_spawn_as_last_hunter() {
    const uint32_t desc = g_cpu.R[0];

    /* THE FORK'S PARTY OF ONE, DECIDED HERE - before anything is built.
     *
     * The hidden co-op partner still existed, merely invisible: it could be
     * punched, damaged and killed, and it respawned. The fork removed the
     * companion AFTER this builder had made it
     * (lsw2_cheats_remove_party_member: clear its presence bit, null its party
     * slot), and the actor stayed in the level's actor list - updated, hit,
     * killed and respawned, invisibly. A write watch on party slot 1
     * (0x0210935C) through an offline lobby boot named this builder as its
     * source, once per level load.
     *
     * The builder already has a "no member here" answer: r0 = 0, which its own
     * co-op path gives when two players are present (0x02050CB4: CMP r2,#2 /
     * MOVHS r0,#0), and which every caller handles. This gives the same answer
     * for the companion, by the rule lsw2_fork_enforce_party_of_one states: a
     * slot is the companion's unless a player sits in it. The player count is
     * the cartridge's own input count ([0x021027F8 + 0x29]) - game state that
     * every console of a lockstep session holds identically at this build - so
     * all of them decide alike. Every level, not only the lobby: the
     * companion is refused wherever the fork runs. The after-the-fact removals
     * stay for any member built by another route. */
    if (ui_fork_build() && desc >= 0x02000000u && desc < 0x02400000u) {
        const int slot = int(read_guest_byte(desc + 9u));
        /* SIGNED: a session's teardown (FUN_02072BC8) leaves the count at -1,
         * and a lobby reloaded after it must still be a party of one. */
        const int players = int(int8_t(
            read_guest_byte(kLsw2NetCountObject + kLsw2NetPlayerCount)));
        if (slot >= 1 && slot >= players) {
            static int said = 0;
            if (said < 8) {
                ++said;
                std::fprintf(stderr,
                             "[fork] party of 1: the level's companion for slot "
                             "%d (character %u) is not built - %d player(s), "
                             "level %d\n", slot,
                             unsigned(read_guest_byte(desc + 8u)), players,
                             lsw2_cheats_current_level());
            }
            g_cpu.R[0] = 0u;
            g_cpu.R[15] = g_cpu.R[14];
            return;
        }
    }

    if (ui_fork_build() && desc >= 0x02000000u && desc < 0x02400000u &&
        lsw2_cheats_current_level() == kLsw2ArenaLobbyLevel) {
        Lsw2ForkSession& session = lsw2_fork_session();
        const uint8_t slot = read_guest_byte(desc + 9u);
        if (slot == 0u && !lsw2_vwh_multiview()) {
            /* SLOT 0 WHILE NOBODY IS SEATED - the solo player as their last
             * hunter. Not "offline" any more: a
             * joiner's lobby is rebuilt at the join, before it is seated, and a
             * host's at Start Lobby, and both were built as the level's Greedo
             * because the session state was no longer Offline. Until the game's
             * multi-view bit is up the world is this console's own and slot 0
             * is this player; once it is up slot 0 is the host on every
             * console, and each console's hunter comes back through the game's
             * switch message instead (lasthunter_observe.cpp). */
            const int hunter = lsw2_last_hunter();
            if (hunter >= 0 && hunter != int(kLsw2CharacterNone)) {
                const uint8_t was = read_guest_byte(desc + 8u);
                if (int(was) != hunter) {
                    write_guest_byte(desc + 8u, uint8_t(hunter));
                    static bool said = false;
                    if (!said) {
                        said = true;
                        std::fprintf(stderr,
                                     "[fork] lobby spawn-as: building the lobby player "
                                     "as the last hunter id %d directly (descriptor "
                                     "said %d) - no Greedo, no switch.\n",
                                     hunter, int(was));
                    }
                }
            }
        } else if (slot >= 1u && slot <= 3u &&
                   session.state() != Lsw2SessionState::Offline) {
            /* PEER SLOTS - the remote-avatar seat. The lobby data
             * spawns slots 1..N as the default Greedo companion; in a session a
             * seated peer owns the slot, so build it as the peer's PUBLISHED
             * character instead. That turns the "joined peer shows up as the
             * single-player co-op buddy" symptom into the peer appearing as its
             * own chosen hunter - visible and distinct - through the game's own
             * KIND-0 builder rather than a memory poke. A peer that has not yet
             * put a character on the wire (chr < 0) is left as-is; the room
             * rebuild on the next join/leave re-runs this builder and catches it.
             * The upper bound matches the cartridge's own seat-loop guard
             * (charid < 0x54) so a garbled wire byte can never build a bad id. */
            const int chr = session.peer_slot_character(int(slot));
            if (chr >= 0 && chr < 0x54 && chr != int(kLsw2CharacterNone)) {
                const uint8_t was = read_guest_byte(desc + 8u);
                if (int(was) != chr) {
                    write_guest_byte(desc + 8u, uint8_t(chr));
                    static int said = 0;
                    if (said < 8) {
                        ++said;
                        std::fprintf(stderr,
                                     "[fork] peer avatar: building party slot %u as "
                                     "the seated peer's hunter id %d (descriptor "
                                     "said %d) - the remote player, not a companion.\n",
                                     unsigned(slot), chr, int(was));
                    }
                }
            }
        }
    }
    /* A SESSION BUILD IS THE LEVEL'S, NOT THE PLAYER'S: slot 0 keeps the
     * level data's character here (only the offline branch above rewrites
     * it). The last-hunter tick restores this console's own station through
     * the game's switch message once the build has settled. */
    if (ui_fork_build() && desc >= 0x02000000u && desc < 0x02400000u &&
        lsw2_vwh_multiview())
        lsw2_last_hunter_note_build(int(read_guest_byte(desc + 9u)));
    g_cpu.R[15] = 0x02050C44u;
    lsw2_arm9_FUN_02050c44();
}

/* FOUR HUNTERS: the host's WM parent is started with room for
 * ONE child = 2 players. The beacon's accept/vacancy flag (record 0x0210B460
 * +0x4B) was measured flipping to "full" the moment the first child joins, so a
 * second child's scan CORRECTLY drops a full host. FUN_020ad970 is the ParentParam
 * setup (0 direct callers, vtable/pointer-dispatched, so an override
 * fires); its param_2 (R1, stored at ctx+0x44) is the host child-capacity that feeds
 * that beacon field. Log it, and force it higher via LSW2_FORCE_MAXENTRY (1..15) to
 * test whether the host then admits 3 children (4 players). Delegates to the
 * cartridge's own lifted body, so unforced it changes nothing but the log. */
extern "C" void lsw2_arm9_FUN_020ad970(void);
void parentparam_maxentry_override() {
    static const int forced = [] {
        const char* e = std::getenv("LSW2_FORCE_MAXENTRY");
        const int v = e ? std::atoi(e) : 0;
        return (v >= 1 && v <= 15) ? v : 0;
    }();
    /* Run the cartridge's own ParentParam setup UNCHANGED: param_2 is the host's
     * max-PLAYERS (=4) and is required - forcing it broke the WM. The WM
     * child-capacity (maxEntry) that feeds the beacon's accept/full flag is a
     * SEPARATE field the setup hardcodes to 1 (struct+0x8e), which is why the host
     * advertises "full" after ONE child. */
    g_cpu.R[15] = 0x020AD970u;
    lsw2_arm9_FUN_020ad970();
    /* DIAGNOSE the write mechanism: DAT_020adae0 (@0x020ADAE0) holds the ParentParam
     * struct base; +0x8e is the maxEntry the setup hardcodes to 1. Read the struct via
     * BOTH the ARM9 (dev 0) and ARM7 (dev 9) bus views, write +0x8e via BOTH, then
     * read back to confirm the write took (a C-global copy would not change). Log
     * every call so the real Start-Lobby setup (where +0x8e==1) is visible vs boot. */
    auto rd16 = [](int dev, uint32_t a) {
        return unsigned((bus_device_read32(dev, a & ~3u) >> ((a & 2u) * 8u)) & 0xFFFFu);
    };
    auto wr16 = [](int dev, uint32_t a, unsigned v) {
        const uint32_t wa = a & ~3u, sh = (a & 2u) * 8u;
        const uint32_t w = bus_device_read32(dev, wa);
        bus_device_write32(dev, wa, (w & ~(0xFFFFu << sh)) | ((v & 0xFFFFu) << sh));
    };
    const uint32_t base0 = bus_device_read32(0, 0x020ADAE0u);
    const uint32_t base9 = bus_device_read32(9, 0x020ADAE0u);
    const uint32_t base = base0;
    if (base < 0x02000000u || base >= 0x02400000u) return;
    const uint32_t a8e = base + 0x8Eu, a90 = base + 0x90u;
    const unsigned d0_8e = rd16(0, a8e), d9_8e = rd16(9, a8e);
    if (forced) { wr16(0, a8e, unsigned(forced)); wr16(9, a8e, unsigned(forced)); }
    static int said = 0;
    if (said < 8) {
        ++said;
        std::fprintf(stderr,
                     "[fork] PP#%d base0=%08X base9=%08X | +0x8e d0=%u d9=%u +0x90 d0=%u | "
                     "after write d0=%u d9=%u\n",
                     said, unsigned(base0), unsigned(base9), d0_8e, d9_8e,
                     rd16(0, a90), rd16(0, a8e), rd16(9, a8e));
    }
}

const NdsDispatchEntry kOverridesCore[] = {
    { 0x020AD970u, 0, parentparam_maxentry_override, nullptr },
    { 0x02055E98u, 0, script_command,        nullptr },
    { 0x0205CD20u, 0, string_decode,            nullptr },
    /* THE ARENA CUSTOMIZE MENU'S COMMIT - see lsw2_customization_menu_commit.
     * Vtable-dispatched (zero direct callers), so this fires only on a menu
     * action; it runs the game's commit then records only the menu's whitelisted
     * options (slot-3 headgear + weapon), which is the whole fix for a
     * character's stock parts following between characters. Fork-only. */
    { 0x0207AEF4u, 0, lsw2_customization_menu_commit, nullptr },
    /* The arena lobby keeps the player: disable the cantina exit door at its own
     * contact handler. See
     * lsw2_doorseal_contact - it sets the door's native disable flag (entity+0x3D)
     * before the handler arms any fade or posts a warp. Vtable-dispatched (zero
     * direct callers). Fork + arena lobby only. */
    { 0x02097BCCu, 0, lsw2_doorseal_contact,    nullptr },
    /* Build the solo lobby player as the last-played hunter directly
     * - see lsw2_lobby_spawn_as_last_hunter. Rewrites the lobby actor
     * descriptor's character byte before the KIND-0 builder reads it, so the
     * player is constructed as the hunter (no Greedo, no switch). Zero direct
     * callers (indirect KIND dispatch). Fork + offline + arena lobby only. */
    { 0x02050C44u, 0, lsw2_lobby_spawn_as_last_hunter, nullptr },
    /* The online session's roster rules - see sessionroster.h. The arena
     * build's only call (0x0208CB94) dispatches; of the actor builder's calls,
     * the respawn's (0x020381C8) and the build's first dressing (0x0208987C)
     * dispatch, and those are the only two it acts on - and only with a
     * session roster in force. Every other caller gets the game's function
     * unchanged. */
    { 0x020897F8u, 0, lsw2_sessionroster_arena_build, nullptr },
    { 0x02091B88u, 0, lsw2_sessionroster_spawn, nullptr },
    /* THE FREE PLAY GRID AS A PICKER - see freeplaypick.h. The unlock query's
     * call from the widget's tap handler (0x0206887C, returning to 0x02068880)
     * crosses banks and dispatches; that call is the one place a confirming
     * tap is visible before it confirms. Every other caller - and every tap
     * while no picker is up - gets the game's function unchanged. */
    { 0x02086534u, 0, lsw2_freeplaypick_unlock_query, nullptr },
    /* A hit does not knock a hat off - see hatstay.h. The
     * hit handler's knock-off spawns the flying hat through the spawner, from
     * another shard, so that call dispatches; the override recognises it by its
     * return address and resumes the handler at its epilogue. Every other
     * spawn gets the game's function unchanged. Fork-only. */
    { kLsw2Spawner, 0, lsw2_hatstay_spawn_override, nullptr },
    /* A capture takes the Carbonite and leaves the customization - see
     * carbonitekeep.h. The capture handler's call of the reset to stock is
     * interpreted and reaches the lifted reset through the dispatcher; the
     * override knows it by its return address. Every other reset gets the
     * game's function unchanged. Fork-only. */
    { kLsw2StockAppearance, 0, lsw2_carbonitekeep_override, nullptr },
    /* The pause menu's messages are one player's (sessionpause.h): the pause, the resume and a client's quit act on the
     * sender's console only. One caller, the pump, in another shard - so it
     * dispatches. */
    { kNetMsgDispatch, 0, netmsg_dispatch_override, nullptr },
    /* The online pause (sessionpause.h): the pause state's update, reached
     * only through its vtable, and the call between the manager's update and
     * entity passes, in another shard. */
    { kLsw2PauseStateUpdate, 0, pause_state_update_override, nullptr },
    { kBetweenPasses, 0, between_passes_override, nullptr },
    /* THE ENQUEUE, ONCE, AT THE ONLY ADDRESS THAT DISPATCHES.
     *
     * This used to register 0x02060084 and 0x020600F0 - "both, rather than
     * guessing". Both were inert: their only callers are in the same generated
     * bank, so the recompiler calls them by symbol and the dispatch table is
     * never consulted. 0x02060158 is the varargs wrapper in front of them and
     * has no direct call sites at all. See script_enqueue_wrapper. */
    { 0x02060158u, 0, script_enqueue_wrapper,   nullptr },

    /* THE ONE THAT CAN ACTUALLY FIRE - see state_message's comment for why the
     * two enqueues above cannot, and why this is not a duplicate of them. */
    { 0x0208CFECu, 0, state_message,            nullptr },
};

/* REGISTERED IN BOTH BUILDS, and that is the whole point of it being separate.
 *
 * `kOverridesCore` below is installed only when `core` is true, which outside
 * `LSW2_HLE=1` means only in the fork. The two screens the master server's
 * baseline integration needs named - the Cantina console's and the pause
 * menu's wireless row - exist in the BASELINE: the fork warps past the first
 * (straight to the arena lobby) and hides the second (the pause menu's hidden
 * rows). A
 * fork-only observer would watch the one build in which both subjects have been
 * removed, which is exactly the mistake `statelog.h` was caught making.
 *
 * Safe unconditionally because it is a pure observer that delegates to the
 * cartridge's own code; the baseline's behaviour is unchanged. */
const NdsDispatchEntry kOverridesAlways[] = {
    { 0x0205ECC4u, 0, lsw2_statereq_override,   nullptr },
    /* THE TWO BEHAVIOUR HOOKS - see behaviourhook.h. Pure observers: each
     * prints and then calls the cartridge's own lifted function.
     *
     * Both builds, because the comparison they exist for is the cantina
     * against the arena and the cantina is reachable in both - the same reason
     * kOverridesAlways exists at all.
     *
     * Reachable for the reason they were hard to find: neither has a single
     * direct BL caller anywhere in the image, so the recompiler reaches both
     * through runtime_dispatch. */
    { 0x0204B450u, 0, lsw2_behaviourhook_disable_override, nullptr },
    { 0x0204A938u, 0, lsw2_behaviourhook_enable_override,  nullptr },
    { 0x02057BC8u, 0, lsw2_behaviourhook_setbyte_override, nullptr },
    { 0x0208C888u, 0, lsw2_behaviourhook_matchstep_override, nullptr },
    { 0x02057A28u, 0, lsw2_behaviourhook_enableplain_override,  nullptr },
    { 0x0207641Cu, 0, lsw2_behaviourhook_enablearrive_override, nullptr },

    /* THE LOBBY CO-OP ARROW'S OBSERVER - see markerhook.h. Same shape as the
     * six above: it prints the sprite id and the caller and then runs the
     * cartridge's own routine. Chosen because it has ZERO direct BL callers,
     * which is what makes an override fire at all; the two functions the three
     * earlier attempts at the arrow reached for have three and five, so an
     * override on either could never have run. */
    { 0x0209F640u, 0, lsw2_markerhook_attach_override, nullptr },
    /* THE HEAD-REVEAL ENHANCEMENT, which belongs to BOTH builds: it is an
     * Enhancement setting for both the base recomp and the Bounty Hunter fork.
     *
     * It was registered in kOverridesCore, which outside LSW2_HLE=1 installs
     * only in the fork - so the baseline's customize panel, which applies hats
     * through this very function, would never have been corrected. Third
     * instance of this class: statelog.h was caught after shipping, statereq.h
     * before, this one by audit. tools/sweep_build_gating.py now checks it.
     *
     * 79 of the applier's 80 callers dispatch; the one that does not is
     * 0x02006220, which applies a character's OWN stock hat, where the covering
     * path is already right. See headfix.h. */
    { 0x020063D4u, 0, lsw2_headfix_gear_applier, nullptr },
    /* "SHUT DOWN LOBBY" AND THE COMMUNICATION-ERROR CRASH - see commguard.h for
     * the disassembly and the measurement.
     *
     * The cross-player consistency check dereferences the view manager three
     * times with no null test, and the cartridge's own session end NULLS that
     * pointer by design - so "Shut Down Lobby" reliably branches the ARM9 into
     * unmapped space one line after the teardown returns. Measured with two
     * players seated: 65 unmapped accesses with the shutdown, 0 in a control
     * that seated the same two players and did not shut down.
     *
     * BOTH BUILDS, because the window is opened by the cartridge's own teardown
     * rather than by anything the fork adds.
     *
     * Zero direct BL callers (tools/blscan.py reports 0 raw hits), so every
     * call reaches it through runtime_dispatch and this override actually
     * fires - the same property the behaviour hooks above were chosen for. */
    { kLsw2NetConsistencyCheck, 0, lsw2_commguard_consistency_check, nullptr },
    /* MOVED OUT OF kOverridesCore FOR THE TOP-SCREEN PORTRAIT FIX. Its forcing
     * is fork-only and stays so - `forced` requires g_force_free_play and the
     * fork's roster wish, so in the baseline this returns the game's own answer
     * unchanged - but the WIDGET CAPTURE above it must happen in both builds,
     * because the portrait fix is an Enhancement in both builds and the Free
     * Play screen exists in both. Registering it here changes nothing the baseline can observe except
     * that the pointer is now known. */
    { 0x02086204u, 0, flag_test_array_c,        nullptr },
    /* The per-frame guest-thread hook the "Shut Down Lobby" return and the
     * lobby respawn need. A probe today; nothing is hung off it until it is
     * proven to fire. */
    { 0x020B0410u, 0, netmsg_pump_probe,       nullptr },
    /* The hook whose guard survives the player count dropping.
     * A probe until it is measured firing. */
    { 0x020B07E8u, 0, netgame_tick_probe,      nullptr },
    { 0x0202C218u, 0, frame_cand1_probe,     nullptr },
    /* The top-screen portrait hop's host, by name - see hud_step_hop_host. In
     * both builds because the portrait fix is an Enhancement in both; the hop
     * itself still declines in the baseline, which is the baseline's open half
     * of that fix. */
    { kLsw2HudStep, 0, hud_step_hop_host,     nullptr },
    { 0x0204A620u, 0, frame_cand2_probe,     nullptr },
    { 0x02055530u, 0, frame_cand3_probe,     nullptr },
    { 0x02088014u, 0, freeplay_message_probe,  nullptr },
};

const NdsDispatchEntry kOverridesWireless[] = {
    { 0x020AD310u, 0, wh_get_system_state,      nullptr },
    { 0x020AD7F0u, 0, wh_step_data_sharing,     nullptr },
    { 0x020AD884u, 0, wh_get_shared_data_address, nullptr },
};

/* LSW2_WH_TRACE=1 - OBSERVE-AND-FORWARD PROBES ON THE GAME'S WIRELESS SURFACE.
 *
 * Measurement for the host-level WH replacement. The game reaches the wireless helper only through
 * the C++ manager at *(0x0210B348), vtable 0x020FFD50 (26 methods, all called
 * virtually), plus WH_GetSystemState by BL. A host-level replacement has to
 * reproduce the exact sequence a real host/join/match makes - which methods,
 * which arguments, which state the game waits for next - so each probe logs
 * the call and then runs the ORIGINAL, unchanged. Per-frame methods log their
 * first calls and then only when an argument or the result changes. */
struct WhtStat { uint32_t count = 0, last_r1 = 0, last_r2 = 0, last_ret = 0; int lines = 0; };
WhtStat g_wht[64];

void wht_forward(int idx, uint32_t addr, const char* name, void (*fn)()) {
    const uint32_t r0 = g_cpu.R[0], r1 = g_cpu.R[1], r2 = g_cpu.R[2],
                   r3 = g_cpu.R[3], lr = g_cpu.R[14];
    const uint32_t st_before = bus_device_read32(9, 0x0210B340u);
    g_cpu.R[15] = addr;
    fn();
    const uint32_t ret = g_cpu.R[0];
    WhtStat& s = g_wht[idx];
    ++s.count;
    const bool changed = s.count == 1 || r1 != s.last_r1 || r2 != s.last_r2 ||
                         ret != s.last_ret;
    s.last_r1 = r1; s.last_r2 = r2; s.last_ret = ret;
    if ((s.count <= 6 || changed) && s.lines < 400) {
        ++s.lines;
        std::fprintf(stderr,
                     "[whtrace] %s(%08X) r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X "
                     "-> %08X  wh %u->%u  game %d  n=%u\n",
                     name, unsigned(addr), unsigned(r0), unsigned(r1),
                     unsigned(r2), unsigned(r3), unsigned(lr), unsigned(ret),
                     unsigned(st_before), unsigned(bus_device_read32(9, 0x0210B340u)),
                     lsw2_state_current(), unsigned(s.count));
    }
}

#define WHT(idx, hex, name)                                                     \
    extern "C" void lsw2_arm9_FUN_##hex(void);                                 \
    void wht_##hex() { wht_forward(idx, 0x##hex##u, name, lsw2_arm9_FUN_##hex); }
WHT(0,  020ad438, "s0 WH_Initialize")
WHT(1,  020ad5dc, "s1 WH_Finalize")
WHT(2,  020ad55c, "s2 WH_Reset")
WHT(3,  020ad970, "s3 WH_ParentConnect")
WHT(4,  020adbcc, "s4 WH_ChildConnect")
WHT(5,  020adc40, "s5 kick(WM_Disconnect)")
WHT(6,  020ad7f0, "s6 WH_StepDS")
WHT(7,  020ad8a4, "s7 WH_GetLastError")
WHT(8,  020ad75c, "s8 WH_EndDataSharing")
WHT(9,  020ad884, "s9 WH_GetSharedDataAdr")
WHT(10, 020ad8cc, "s10 WH_GetBitmap")
WHT(11, 020adc78, "s11 WH_GetCurrentAid")
WHT(12, 020ad918, "s12 setGgid")
WHT(13, 020af204, "s13 getGgid")
WHT(14, 020ad954, "s14 WH_GetMeasureChannel")
WHT(15, 020adaf0, "s15 WH_StartMeasureChannel")
WHT(16, 020adb5c, "s16 WH_StartScan")
WHT(17, 020ad928, "s17 WH_EndScan")
WHT(18, 020adddc, "s18 set+0x14")
WHT(19, 020adc88, "s19 WM_SetEntry")
WHT(20, 020add78, "s20 WH_End")
WHT(21, 020ad3bc, "s21 WH_ParentParamDefaults")
WHT(22, 020ad8dc, "s22 hostTable")
WHT(23, 020ad8e8, "s23 hostCount")
WHT(24, 020ad534, "s24 WM(020D6E6C)")
WHT(25, 020ad540, "s25 WH_SetUserGameInfo")
WHT(26, 020af9a0, "WM indication callback")
WHT(27, 02071b9c, "fn 02071B9C lobby session")
WHT(28, 020719e0, "fn 020719E0 setup")
WHT(29, 02072620, "fn 02072620 setup")
WHT(30, 0207281c, "fn 0207281C setup")
WHT(31, 02072ad8, "fn 02072AD8 scan")
WHT(32, 02072900, "fn 02072900 host")
WHT(33, 02070248, "fn 02070248 browse")
WHT(34, 02072700, "fn 02072700 join")
#undef WHT

const NdsDispatchEntry kOverridesWhTrace[] = {
    { 0x020ad438u, 0, wht_020ad438, nullptr }, { 0x020ad5dcu, 0, wht_020ad5dc, nullptr },
    { 0x020ad55cu, 0, wht_020ad55c, nullptr }, { 0x020ad970u, 0, wht_020ad970, nullptr },
    { 0x020adbccu, 0, wht_020adbcc, nullptr }, { 0x020adc40u, 0, wht_020adc40, nullptr },
    { 0x020ad7f0u, 0, wht_020ad7f0, nullptr }, { 0x020ad8a4u, 0, wht_020ad8a4, nullptr },
    { 0x020ad75cu, 0, wht_020ad75c, nullptr }, { 0x020ad884u, 0, wht_020ad884, nullptr },
    { 0x020ad8ccu, 0, wht_020ad8cc, nullptr }, { 0x020adc78u, 0, wht_020adc78, nullptr },
    { 0x020ad918u, 0, wht_020ad918, nullptr }, { 0x020af204u, 0, wht_020af204, nullptr },
    { 0x020ad954u, 0, wht_020ad954, nullptr }, { 0x020adaf0u, 0, wht_020adaf0, nullptr },
    { 0x020adb5cu, 0, wht_020adb5c, nullptr }, { 0x020ad928u, 0, wht_020ad928, nullptr },
    { 0x020adddcu, 0, wht_020adddc, nullptr }, { 0x020adc88u, 0, wht_020adc88, nullptr },
    { 0x020add78u, 0, wht_020add78, nullptr }, { 0x020ad3bcu, 0, wht_020ad3bc, nullptr },
    { 0x020ad8dcu, 0, wht_020ad8dc, nullptr }, { 0x020ad8e8u, 0, wht_020ad8e8, nullptr },
    { 0x020ad534u, 0, wht_020ad534, nullptr }, { 0x020ad540u, 0, wht_020ad540, nullptr },
    { 0x020af9a0u, 0, wht_020af9a0, nullptr }, { 0x02071b9cu, 0, wht_02071b9c, nullptr },
    { 0x020719e0u, 0, wht_020719e0, nullptr }, { 0x02072620u, 0, wht_02072620, nullptr },
    { 0x0207281cu, 0, wht_0207281c, nullptr }, { 0x02072ad8u, 0, wht_02072ad8, nullptr },
    { 0x02072900u, 0, wht_02072900, nullptr }, { 0x02070248u, 0, wht_02070248, nullptr },
    { 0x02072700u, 0, wht_02072700, nullptr },
};

}  // namespace

/* See fork.h. */
const char* lsw2_fork_character_name(int character_id, char* buf, size_t n) {
    if (!buf || n == 0) return "";
    buf[0] = '\0';

    /* Names are decoded through 0x1CB + id. */
    constexpr uint32_t kCharacterNameBase = 0x1CBu;
    if (character_id >= 0) {
        const Lsw2StringResult r = lsw2_string_decode(
            kLsw2StringControlBlock, kCharacterNameBase + uint32_t(character_id),
            buf, uint32_t(n), guest_read8, nullptr);
        if (r != Lsw2StringResult::NotReady && buf[0] != '\0') return buf;
    }

    /* THE BANK IS NOT ALWAYS LOADED, and a blank row would look like a bug in
     * the picker rather than in the moment it was opened. Say what it is. */
    std::snprintf(buf, n, "Character %d", character_id);
    return buf;
}


/* The host settings menu's Extras names, from the cartridge rather than from a
 * table.
 *
 * The eighteen Extras are strings 0x243-0x254 and the flag index is
 * `id - 0x243`, so the game already holds every name the host menu shows.
 * `hostsettings.cpp` used to carry them as literals, which made a ROM-free,
 * testable file quietly answerable to the string bank - the same shape
 * `lsw2_fork_character_name` above exists to avoid.
 *
 * Same fallback rule as that one: when the bank is not up the decode returns
 * nothing, and hostsettings keeps its built-in name rather than drawing a blank
 * row that would look like a bug in the menu instead of in the moment it was
 * opened. */
const char* lsw2_fork_extra_name(int flag, char* buf, size_t n) {
    if (!buf || n == 0) return "";
    buf[0] = '\0';
    if (flag < 0 || flag >= kLsw2ExtrasCount) return buf;

    constexpr uint32_t kExtraNameBase = 0x243u;
    const Lsw2StringResult r = lsw2_string_decode(
        kLsw2StringControlBlock, kExtraNameBase + uint32_t(flag),
        buf, uint32_t(n), guest_read8, nullptr);
    if (r == Lsw2StringResult::NotReady) buf[0] = '\0';
    return buf;
}


/* WHAT THE FORK RENAMES, and nothing else.
 *
 * The online menus replace the three arena menu entries; the pause menu hides
 * three entries. Both are the same mechanism - answer the id differently - and
 * both are listed here so "what text does the fork change" is one table rather
 * than a search.
 *
 * THE BLANKS ARE NOT NECESSARILY UNSELECTABLE. The row width is
 * measured from the label, so an empty label draws nothing - but the cursor may
 * still stop there, and a player pressing A on nothing is worse than a visible
 * entry. That is a question for play, and if it goes the wrong way the fix is to
 * refuse each entry's action rather than to widen this table.
 */
struct ForkString { uint32_t id; const char* text; };
const ForkString kForkStrings[] = {
    /* The online arena menu, onto the three entries the game already has. */
    { 0x063u, "Host Internet Game" },      /* was "Host Multi-Card Game"   */
    { 0x064u, "Join by IP/Room Code" },    /* was "Host Single-Card Game"  */
    { 0x065u, "View Master Server" },      /* was "Join Wireless Game"     */
    /* Hidden from the pause menu. Empty, so the row measures to nothing. */
    { 0x03Cu, "" },                        /* Host Wireless */
    { 0x02Cu, "" },                        /* Save Game     */
    /* AND THE EXTRAS ROW, which is 0x007 - read out of the pause menu's own id
     * table at 0x020F4D00 rather than chosen from three static candidates.
     * That table is the menu saying which id it uses, so the guess is retired.
     *
     * 0x007 is also the front end's Extras label, which is why it was left out
     * before. In the fork that costs nothing: the boot warps straight into the
     * Arena Lobby and the sealed exit door keeps the player there, so the front end is
     * never drawn. Blanking it in the BASELINE would be wrong, and the baseline
     * installs none of this table. */
    { 0x007u, "" },                        /* Extras        */
};

/* (The online join and disconnect popups were raised from here, through a
 * script queue and then the game's message display; neither reached the
 * screen in a networked match. popuphud.h draws them now, and the sink below
 * points there.) */

/* The pause menu's row hiding, kept true for as long as the menu is on screen.
 *
 * The icon was still drawn and the cursor still resting on hidden rows. Both because the corrections ran once, at build time, from
 * inside the string-decode override - and the player scrolls afterwards. */
/* The online pause-menu relabel.
 *
 * While connected to a server, the pause menu's Quit Game row reads
 * Disconnect; for hosts it reads Shut Down Lobby instead, the same as at the
 * arena console entity.
 *
 * ROW 5's STRING ID IS 0x044, derived rather than guessed. The builder fills a
 * six-entry array from a table through `r2`: row 1 is `[r2+0x0A]` and row 3 is
 * `[r2+0x0E]`, and those are the ids the fork already blanks successfully
 * (0x03C Host Wireless, 0x007 Extras). That pins `r2` at 0x020F4CF8, and row 5
 * - `[r2+0x12]` - reads 0x044. Row 4 is the odd one out: the builder stores a
 * literal 0x2C there conditionally, which is exactly the Save Game id the fork
 * blanks, so the derivation is confirmed at both ends.
 *
 * THE LABEL IS ONE HALF; THE ACTION IS lsw2_fork_disconnect_backstop().
 *
 * What a row DOES is the six-way jump table on the cursor at 0x0208EA6C, which
 * an override would have to replace whole - so the row is not intercepted.
 *
 * AN EARLIER VERSION OF THIS COMMENT CLAIMED the fork's level pin already kept
 * the player in the lobby, so the row could not quit while connected. THAT WAS
 * FALSE, and checking it is what turned this from a rename into a feature:
 * `lsw2_cheats_pin_level` starts with
 *
 *     if (read32(args) != kLevelStateKind) continue;   // not a level change
 *
 * and Quit Game does not request a level - it requests the front end. The pin
 * never sees it. A row labelled "Disconnect" that actually quits to a title
 * screen the fork exists to skip would have been worse than leaving it alone.
 *
 * fork.cpp closes it from the other end instead: if a session is up and the
 * player ends up somewhere that is neither the lobby nor an arena, that is the
 * quit, and the fork drops the session and warps back, returning the player to
 * the lobby offline. */
/* Quit Game's string id is pauselayout.h's kLsw2PauseIdQuitGame, which that
 * header declares alongside the other five row ids read from 0x020F4CF8. */

void pause_quit_relabel() {
    Lsw2ForkSession& session = lsw2_fork_session();

    const char* want = nullptr;
    if (session.in_session())
        want = session.is_host() ? "Shut Down Lobby" : "Disconnect";

    /* ONLY ON CHANGE. Dropping the cached decode is the expensive half, and the
     * answer is the same on almost every frame. */
    static const char* applied = nullptr;
    if (want == applied) return;
    applied = want;

    lsw2_string_set_override(kLsw2PauseIdQuitGame, want);

    /* AND DROP THE CACHED DECODE, for the same reason the popup path does: the
     * cache is keyed by id and this id has just changed meaning. */
    if (kLsw2PauseIdQuitGame < kStringCacheIds &&
        g_string_cache[kLsw2PauseIdQuitGame]) {
        std::free(g_string_cache[kLsw2PauseIdQuitGame]);
        g_string_cache[kLsw2PauseIdQuitGame] = nullptr;
    }

    std::fprintf(stderr, "[fork] pause row 5 is now \"%s\"\n",
                 want ? want : "Quit Game");
}

/* ONLINE: WHILE CONNECTED, OPENING THE PAUSE MENU LEAVES GAMEPLAY RUNNING.
 *
 * This was once recorded as blocked on an opcode that "cannot be picked out of
 * the image; it is whichever one runs" - true of intercepting the pause REQUEST,
 * and irrelevant, because the port does not need to stop the pause from being
 * asked for. It needs gameplay to keep running, and that is a bit.
 *
 * `0x02093C6C` is the whole of it:
 *
 *     r1 == 0  ->  [0x027E02A8] &= ~3
 *     else     ->  [0x027E02A8] |=  3
 *
 * and the pause menu's own Resume - case 0 of the jump table at 0x0208EA6C -
 * calls it with 0. So bits 0|1 of that word ARE the pause, and clearing them is
 * exactly what Resume does, minus closing the menu.
 *
 * TREATED AS A PAIR, because the game treats them as a pair - and that is
 * MEASURED rather than assumed. Of the 194 literal references to this word, the
 * masks actually tested are:
 *
 *     0x40  x80   the multi-view bit
 *     0x200 x29
 *     0x800 x25
 *     0x03  x11   <- these two, ALWAYS together
 *     0x30  x5, and four singletons
 *
 * **No site anywhere tests bit 0 or bit 1 on its own.** So the pair has no
 * hidden per-bit meaning to trample, and clearing both is precisely what
 * 0x02093C6C does. Worth checking before writing a global that 194 sites read;
 * it would have been expensive to discover later.
 *
 * ONLY WHILE CONNECTED, which is the requirement's own condition - a solo player
 * pausing should still pause. And only while the menu is up, which is where
 * this runs from. */
constexpr uint32_t kGameStateBits = kLsw2GameStateBits;
constexpr uint32_t kPausedBits = 3u;

void pause_keep_running() {
    if (!lsw2_fork_session().in_session()) return;

    /* ONLY DURING AN ACTUAL ARENA MATCH, not in the lobby menus. Keeping
     * gameplay running when the pause menu is up is what stops a networked
     * MATCH freezing while one player pauses. But the fork's own lobby menus -
     * the host settings menu above all - are opened from that same pause path
     * while the host is standing in the arena LOBBY (level 9, not an arena map),
     * and there un-pausing left the host's character live: navigating the menu
     * with the fire button drew and fired the weapon, where stock LSW2 (its
     * bounty-hunter menu, co-op lobby and Cantina store) keeps the character
     * suppressed. So the keep-running override is confined to an arena map; in
     * the lobby the paused bits stand and the character is frozen, as vanilla
     * menus leave it. */
    if (!lsw2_cheats_in_arena_match()) return;

    const uint32_t now = bus_device_read32(9, kGameStateBits);
    if (!(now & kPausedBits)) return;

    bus_device_write32(9, kGameStateBits, now & ~kPausedBits);

    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr,
                     "[fork] connected - the pause menu is open but gameplay "
                     "keeps running\n");
    }
}

void lsw2_pause_tick() {
    if (!ui_fork_build()) return;

    /* BEFORE THE MENU-OBJECT CHECK. The label has to be right when the menu is
     * BUILT, and the build is the only moment the row's string is read - so
     * waiting until a menu object is known would be a frame too late. */
    pause_quit_relabel();

    /* Online, the pause menu leaves gameplay running. Runs only when a
     * menu object is live, which is the loop below - see pause_keep_running. */

    if (!g_pause_obj) return;
    if (!pause_menu_still_live()) {
        g_pause_obj = 0;
        return;
    }
    pause_apply(g_pause_obj);
    pause_keep_running();
}



/* SORT EVERY TABLE BEFORE REGISTERING IT - the framework BINARY-SEARCHES.
 *
 * THE BUG THIS FIXES, and it was latent from the first override this port ever
 * registered. `lookup_in` in the framework's runtime_arm.cpp is a binary search
 * over the bank's entries:
 *
 *     while (lo < hi) {
 *         unsigned mid = (lo + hi) >> 1u;
 *         if (table[mid].addr < pc) lo = mid + 1u;
 *         else                      hi = mid;
 *     }
 *
 * That is only correct on a SORTED array. Every table in this file was written
 * in the order the overrides were thought of - kOverridesAlways starts
 * 0x0205ECC4, 0x020063D4, 0x02086204 - so the search has been walking an
 * unsorted array and finding SOME entries while missing others, with which ones
 * depending on the order and the length.
 *
 * HOW IT SURFACED. An override on 0x0202C218 fired sixty
 * times a second with four entries registered and NOT AT ALL with one or three,
 * and the difference was an unrelated address that never fired itself. That is
 * exactly what an unsorted binary search does - adding an element moves the
 * midpoints. The dispatch histogram then proved the dispatcher WAS being asked
 * about 0x0202C218 five times in three frames while its override never ran, so
 * the miss was in the LOOKUP and not in the game.
 *
 * WHAT ELSE THIS MAY EXPLAIN, and it is not claimed until measured: several
 * overrides in this project have been recorded as "registered and never fired"
 * and eliminated on that basis - the two state-update slots and state 48's
 * +0x14 among them. A lookup that silently misses is
 * exactly the failure that would produce those reports.
 *
 * WHY SORT HERE RATHER THAN HAND-ORDERING THE TABLES. A hand-sorted table is
 * one careless insertion away from being wrong again, silently, with the only
 * symptom being a feature that stops working in a later build. Sorting at
 * registration cannot be forgotten. */
constexpr unsigned kSortedArena = 96;
NdsDispatchEntry g_sorted[kSortedArena];
unsigned g_sorted_used = 0;

void register_sorted(const NdsDispatchEntry* table, unsigned len,
                     uint32_t exc_base, const char* what) {
    if (len == 0) return;
    if (g_sorted_used + len > kSortedArena) {
        /* SAY SO RATHER THAN REGISTER AN UNSORTED TABLE. A silent fallback here
         * would reintroduce the exact bug this exists to remove. */
        std::fprintf(stderr,
                     "[hle] dispatch: NO ROOM to sort %s (%u entries) - "
                     "registering UNSORTED, lookups may miss\n", what, len);
        nds_register_dispatch(NDS_ARM9, table, len, exc_base);
        return;
    }

    NdsDispatchEntry* out = &g_sorted[g_sorted_used];
    for (unsigned i = 0; i < len; ++i) out[i] = table[i];

    /* Insertion sort - these tables are a dozen entries, and a stable sort
     * keeps duplicate addresses in their registered order, which is what
     * lookup_in's "scan forward while addr matches" expects. */
    for (unsigned i = 1; i < len; ++i) {
        NdsDispatchEntry key = out[i];
        unsigned j = i;
        while (j > 0 && out[j - 1].addr > key.addr) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }

    g_sorted_used += len;
    nds_register_dispatch(NDS_ARM9, out, len, exc_base);
}

extern "C" void lsw2_register_dispatch_overrides(const char* rom_sha1) {
    (void)rom_sha1;

    /* THE FORK FLAG COMES FROM THE ENVIRONMENT HERE, NOT FROM ui_fork_build().
     *
     * This function runs from main(), before the title banks and therefore
     * before our shell exists - and `ui_set_fork_build()` is called by the
     * shell. So `ui_fork_build()` is still FALSE at this point, always, in both
     * executables. The fork's menu-string overrides were guarded on it and had
     * consequently never been installed even when the table was switched on.
     *
     * main() sets `LSW2_FORK` immediately after working the build out from
     * argv[0], well before this call, precisely so that code running this early
     * has an answer. Use it. */
    const char* fork_env = std::getenv("LSW2_FORK");
    const bool fork = fork_env && *fork_env == '1';

    /* THE FORK TURNS THE CORE OVERRIDES ON BY ITSELF.
     *
     * They were opt-in behind LSW2_HLE=1, and nothing set it - not launch.sh,
     * not PlayLSW2.bat, not the exe. So in every ordinary launch the whole
     * table was absent: no relabelled arena menu, no forced roster mode, no
     * decoder, and no trace. Several features read as unimplemented when they
     * were merely never installed, which is a worse failure than a broken one
     * because there is nothing to debug.
     *
     * The baseline keeps them off, and that is the point rather than caution:
     * the base recomp plays the same as it normally does. The fork is the
     * build whose features require them.
     *
     * LSW2_HLE still wins in both directions - =1 forces them on in the
     * baseline, =0 forces them off in the fork, which is the switch that makes
     * an override that turns out to be wrong one restart away from gone. */
    const char* on = std::getenv("LSW2_HLE");
    const bool core = on ? (*on == '1') : fork;
    const bool wireless = on && *on == '1';
    /* THE HOST-LEVEL WH, FIRST, so its answers win over every older
     * hook on the same wireless-manager methods (the ParentParam logger at
     * 0x020AD970, the WH readiness poll at 0x020AD438): a dispatch lookup takes
     * the earliest registered table that holds the address. Fork only - the
     * baseline's co-op still rides the radio and overrides the connect for its
     * own lobby list. On in the fork unless LSW2_VWH=0. */
    if (fork) {
        unsigned n = 0;
        if (const NdsDispatchEntry* vwh = lsw2_vwh_overrides(&n)) {
            register_sorted(vwh, n, 0xFFFF0000u, "kVwhOverrides");
            std::fprintf(stderr, "[vwh] host-level WH on: %u wireless-manager "
                         "methods answered by the PC session\n", n);
        }
    }
    /* BEFORE THE FORK-ONLY RETURN. See kOverridesAlways. */
    register_sorted(kOverridesAlways,
                    unsigned(sizeof kOverridesAlways /
                             sizeof *kOverridesAlways),
                    0xFFFF0000u, "kOverridesAlways");

    /* The master server's BASELINE integration: its lobby list joins over TCP,
     * so this belongs before the fork-only return and not after it.
     *
     * IT USED TO SIT AFTER, forty lines below, and the effect was the reverse
     * of its own comment in both directions: `core` is `fork` outside
     * LSW2_HLE, so the baseline never installed the override its list needs,
     * and the fork installed the one its own comment says would "put a second,
     * different list in front of the same player". The internal
     * `if (ui_fork_build()) return;` that was supposed to prevent the second
     * half could not, because ui_fork_build() is false here in both
     * executables - which THIS function's own header comment states, forty
     * lines above where the call used to be. The flag is passed in now. */
    lsw2_coopjoin_install(fork);

    if (!core) return;

    /* NDS_ARM9, NOT 9 - and the two APIs in this file disagree on purpose.
     *
     * nds_register_dispatch takes an NdsCpu enum where NDS_ARM9 == 0, and does
     * g_ctx[cpu & 1]. Passing 9 masks to 1, which is the ARM7: the table was
     * registered on the wrong processor and neither override was ever reached.
     * The registration line printed happily the whole time.
     *
     * bus_device_read32 above takes 9 for the same processor. Two neighbouring
     * calls, two different conventions, and only one of them complains. */
    /* THE FORK'S TEXT, installed before the banks so the first menu drawn
     * already has it. Baseline builds install nothing and every id falls
     * through to the bank, which is what "the base plays as it normally does"
     * requires. */
    if (fork) {
        /* The popup sink, so the session can announce arrivals without knowing
         * anything about the guest. See forkpopup.h. */
        lsw2_set_popup_sink(lsw2_popup_hud_raise);

        /* The online host's character picker asks for sixty-five names; this is where the
         * cartridge's own decoder gets handed to it. Same shape as the popup
         * sink above, and for the same reason - characterpick.cpp must not
         * depend on the guest. */
        lsw2_set_character_name_source(lsw2_fork_character_name);
        /* The host settings menu's Extras names come from the cartridge too. */
        lsw2_set_extra_name_source(lsw2_fork_extra_name);

        for (const ForkString& f : kForkStrings)
            lsw2_string_set_override(f.id, f.text);
        std::fprintf(stderr, "[fork] %u menu strings replaced\n",
                     unsigned(sizeof kForkStrings / sizeof *kForkStrings));
    }

    register_sorted(kOverridesCore,
                    unsigned(sizeof kOverridesCore / sizeof *kOverridesCore),
                    0xFFFF0000u, "kOverridesCore");
    /* THE WH READINESS POLL, only when the bring-up is asked for.
     *
     * REGISTERED HERE AND NOT IN A TABLE because it is conditional on an env
     * var the tables cannot see, and because an override that answers a
     * question differently from the cartridge has no business being installed
     * in a run that did not ask for it. See whsession.h. */
    if (fork && lsw2_wh_enabled()) {
        static const NdsDispatchEntry kWhReady[] = {
            { kLsw2WhReadyPoll,   0, lsw2_wh_ready_poll,   nullptr },

            { kLsw2ChooserEnter,  0, lsw2_wh_probe_update, nullptr },
        };
        register_sorted(kWhReady,
                        unsigned(sizeof kWhReady / sizeof *kWhReady),
                        0xFFFF0000u, "kWhReady");
        /* SAY HOW MANY, NOT JUST THAT IT HAPPENED. The table has grown a
         * second entry - state 54's post-construction method - and "the
         * readiness poll is overridden" would read identically whether that
         * one registered or not. A run wondering why an override is silent
         * should be able to rule registration out from the log alone. */
        std::fprintf(stderr,
                     "[hle] WH bring-up ON (default; LSW2_REAL_WH=0 disables): "
                     "%u override(s) - "
                     "%08X readiness poll, %08X state 54 post-ctor\n",
                     unsigned(sizeof kWhReady / sizeof *kWhReady),
                     unsigned(kLsw2WhReadyPoll), unsigned(kLsw2ChooserEnter));
    }

    if (wireless)
        register_sorted(kOverridesWireless,
                        unsigned(sizeof kOverridesWireless /
                                 sizeof *kOverridesWireless),
                        0xFFFF0000u, "kOverridesWireless");
    else if (const char* t = std::getenv("LSW2_WH_TRACE");
             t && *t == '1' && !lsw2_vwh_enabled())
        register_sorted(kOverridesWhTrace,
                        unsigned(sizeof kOverridesWhTrace /
                                 sizeof *kOverridesWhTrace),
                        0xFFFF0000u, "kOverridesWhTrace");

    std::fprintf(stderr,
                 "[hle] %u dispatch override(s) registered BEFORE the title "
                 "banks (%s build, wireless %s)\n",
                 unsigned(sizeof kOverridesCore / sizeof *kOverridesCore +
                          (wireless ? sizeof kOverridesWireless /
                                          sizeof *kOverridesWireless : 0)),
                 fork ? "fork" : "baseline", wireless ? "on" : "off");
}

/* How many times an override actually ran. Reported rather than assumed,
 * because "registered" and "reached" are different claims and this project has
 * already been burned by three commands that were accepted and inert. */
extern "C" uint64_t lsw2_hle_hits() { return g_hits; }

/* The data-sharing counters, same reasoning. A stall count that is climbing is
 * the difference between "the netcode is running" and "the netcode is running
 * and the guest is starving", and only one of those is worth reporting as
 * working. */
extern "C" uint64_t lsw2_hle_step_hits() { return g_step_hits; }
extern "C" uint64_t lsw2_hle_step_stalls() { return g_step_stalls; }

/* The string cache, same reasoning again. These two were incremented on every
 * decode and read by nothing at all - so the cache above could have been
 * missing every time and looked identical from outside. */
extern "C" uint64_t lsw2_hle_string_cache_hits() { return g_cache_hits; }
extern "C" uint64_t lsw2_hle_string_cache_misses() { return g_cache_misses; }
