/* RUNNING THE CARTRIDGE'S GEAR APPLIER ON THE GUEST'S OWN THREAD.
 *
 * PLAIN ENGLISH, AND WHICH DEFECTS THIS SERVES. Two observed defects turn out
 * to be one: customization did not auto-apply on subsequent Free Play switches
 * and character changes, and Jango's and Boba's heads did not render when a
 * headwear item was applied through customize. Both need the cartridge's own
 * gear applier 0x020063D4 to run when the fork asks, and until now it could not.
 *
 * WHY THE OLD WAY CANNOT WORK, measured to the number. Called
 * through `lsw2_guest_call`, the applier reaches an idle wait -
 * `0x020CD73C` is `OS_EnableInterrupts(); while (1) halt;` - and the seam
 * reports:
 *
 *     the pump stopped because NOTHING IS DUE:
 *     now=382 next_event=18446744073709551615
 *
 * That sentinel is UINT64_MAX, which is `nds_next_system_event_time()`'s value
 * for an EMPTY QUEUE. A host guest-call runs between frames, where nothing is
 * scheduled at all - no VBlank, no timer, no device event - so the interrupt
 * the applier waits for can never be raised. It is not a slow wait; there is
 * nothing to wait for. Making the seam wait harder is answering the wrong
 * question, and is not done.
 *
 * SO THE GUEST RUNS IT, on its own thread, where the queue is live and
 * interrupts fire. That is the TRAMPOLINE HOP, the same mechanism that runs the
 * cartridge's session end in whsession.cpp and is verified twice (once for
 * the host, once for the joiner).
 *
 * WHERE THE HOP IS TAKEN FROM, and this is what took four iterations. A hop can
 * only act from inside a DISPATCH, and the two hooks previously proven - the
 * receive pump and the netgame tick - both require a wireless session, which a
 * solo Free Play character switch does not have. Seven candidates were picked
 * by reading the game's code and all seven were refuted by counting them.
 * The problem was then measured instead: a histogram inside
 * `runtime_dispatch` names the addresses the dispatcher is actually consulted
 * for, which is exactly the set an override can hook. Three of them fire sixty
 * times a second in a solo lobby. This module hangs off one.
 *
 * A QUEUE, NOT A CALL, because a hop runs ONE function at a time - it points
 * the guest's PC at the target and waits for it to come back - while the fork
 * dresses several slots at once. Requests are queued and drained one per frame,
 * which at sixty frames a second is imperceptible for a handful of parts.
 *
 * AND THE HEAD FIX COMES FREE. `lsw2_headfix_after_apply` already knows how to
 * re-attach the head for the nine characters whose stock hat covers it; it just
 * has to run after the applier actually ran. It is called on completion here.
 */

#ifndef LSW2_GEARHOP_H
#define LSW2_GEARHOP_H

#include <cstdint>

/* Ask for `part` to be applied to `actor`. Returns false when the queue is full
 * or the request is a duplicate of one already waiting - saying so rather than
 * dropping it silently, because a lost request looks exactly like the bug this
 * exists to fix. */
bool lsw2_gearhop_request(uint32_t actor, uint16_t part, int slot);

/* THE TOP-SCREEN PORTRAIT FOLLOWING A CHARACTER CHANGE, through the same
 * mechanism.
 *
 * The party refresh `0x020691B4(widget, 0)` fails through the SAME seam the
 * gear applier did, for the same reason (between frames the machine's
 * event queue is EMPTY, so a guest function that waits for an interrupt can
 * never complete). It has never once returned.
 *
 * WHAT THIS DOES NOT PROMISE. A measurement found the top-screen portrait to be
 * neither BG tiles nor sprites - it is 3D geometry - and what feeds it is still
 * unknown. So a refresh that finally completes may still not move the portrait.
 * This makes the call possible; whether it is the right call is the next
 * measurement, not a claim. */
bool lsw2_gearhop_request_refresh(uint32_t widget);

/* ANY GUEST CALL, tagged so the caller can tell its own steps apart.
 *
 * WHY THE GENERIC FORM EXISTS NOW. The two entry points above each name one
 * function, which was right while the hop ran one function each. The portrait's fix
 * is a SEQUENCE - release two sprites through their own vtables, clear the
 * slots, rebuild - and the first two targets are not known until the objects
 * are read, because they are virtual destructors. Naming them at compile time
 * is impossible, so the target is a parameter.
 *
 * `tag` comes back to `lsw2_hudportrait_step_returned` on completion. It is
 * needed because each call finishes on a LATER FRAME, so the sequence cannot be
 * written as straight-line code; the completion is where the next step is
 * decided. */
bool lsw2_gearhop_request_call(uint32_t target, uint32_t a0, uint32_t a1,
                               uint32_t a2, int tag);

/* THE SAME CALL, PINNED TO ONE HOST - taken only when the override at `host`
 * is the one asking.
 *
 * WHY THE PHASE MATTERS, measured in two runs. The hop
 * takes a request from whichever hosting override calls first, and the one
 * that fires most is 0x0202C218 - a component update reached from an ACTOR's
 * per-frame step (FUN_02004034). The top-screen portrait's two sprite
 * destructors ran from there, in the middle of the entity update pass, and
 * after a match's return to the lobby the host died between the second
 * destructor and the rebuild: the ARM7 restarted through its own entry code
 * and the ARM9 ran off into garbage. The cartridge deletes those same sprites
 * from INSIDE the HUD object's own update (FUN_0208b0b8, via FUN_0208b54c), a
 * different point in the frame. At boot the two placements are
 * indistinguishable because nothing reuses the freed memory in time; in a
 * live session the wireless layer allocates every frame, and a deletion made
 * where the game never makes one is a use-after-free waiting for a busy heap.
 *
 * So a caller that copies one of the game's own sequences can now also copy
 * WHERE the game runs it. `host` 0 means any host, which is what every other
 * request still asks for. */
bool lsw2_gearhop_request_call_at(uint32_t target, uint32_t a0, uint32_t a1,
                                  uint32_t a2, int tag, uint32_t host);

/* A CALL WITH STACK ARGUMENTS - four registers and three words on the stack,
 * pinned to `host`, its completion handed to lsw2_touchportrait_step_returned
 * with `tag`.
 *
 * For the game's 2D sprite factory (0x0201AD50), which takes seven arguments
 * and is how the touch screen's player portraits are built. The three words go
 * just below the host's sp, as a send's payload does - free stack, since the
 * host has not run its prologue - in the order the callee reads them: s0 at
 * [sp], s1 at [sp+4], s2 at [sp+8]. sp is put back when the call returns. */
bool lsw2_gearhop_request_call7_at(uint32_t target, uint32_t a0, uint32_t a1,
                                   uint32_t a2, uint32_t a3, uint32_t s0,
                                   uint32_t s1, uint32_t s2, int tag,
                                   uint32_t host);

/* A GUEST CALL WHOSE RETURN THE FORK IGNORES - the cartridge's own Extras
 * toggle (the host settings' Extras). Runs on the guest thread like the others, because the
 * toggle re-dresses the party and starts theme music (the scheduler must be
 * live), but nothing is done on completion. */
bool lsw2_gearhop_request_toggle(uint32_t target, uint32_t a0, uint32_t a1,
                                 uint32_t a2);

/* A PLAYER'S OWN ACTION IN A SHARED SESSION - four register arguments, nothing
 * done on completion.
 *
 * For the cartridge's own "this player did something" functions, which in a
 * wireless session do not change the game themselves: they send a netgame
 * message that every console applies at the same dataset. The customize menu's
 * hat and weapon helpers (0x0207B240 / 0x0207B2C8) are the first users - they
 * take the part in r3, which the other kinds never needed. When the call runs
 * on this console does not matter for determinism: all it does is queue the
 * message, exactly as pressing the menu button would. A duplicate (same target
 * and all four arguments) is refused. */
bool lsw2_gearhop_request_action(uint32_t target, uint32_t a0, uint32_t a1,
                                 uint32_t a2, uint32_t a3);

/* THE SAME, PINNED to one host override (the way the portrait's steps are
 * pinned to kLsw2HudStep) - for a call whose PLACE in the frame matters, not
 * only its frame: a change to the simulation queued at the dataset boundary on
 * every console, which must then run at the same point of the same frame
 * everywhere, and not in the middle of whatever loop happens to reach the
 * first host. */
bool lsw2_gearhop_request_action_at(uint32_t target, uint32_t a0, uint32_t a1,
                                    uint32_t a2, uint32_t a3, uint32_t host);

/* SEND ONE OF THE GAME'S OWN NETGAME MESSAGES - type and up to eight payload
 * bytes (two words) - through its own send, 0x020B04F0(0x0210794C, type,
 * payload, 0xFF), on the guest's thread. The payload is written just below the
 * host's sp for the call, which the send copies before it returns (0x020B05A8),
 * so no guest buffer is needed. For a message the game has no function to send
 * with a register argument: the Extras toggle 0x36, sent inline by the Extras
 * menu. Every console, the sender included, handles it at the same exchange. */
bool lsw2_gearhop_request_send(uint8_t type, uint32_t word0, uint32_t word1);

/* Called at the top of the per-frame override that hosts the hop, with that
 * override's own guest address. Returns TRUE when it has just redirected the
 * guest, and the caller MUST return immediately without touching r0-r3 or
 * r15 - it is not being asked to do its own job this time round. */
bool lsw2_gearhop_hook(uint32_t host_entry);

/* From the fork's per-frame tick. Retires an arm nobody ever claims, WITHOUT
 * restoring registers - the precedent is `lsw2_wh_session_end_tick`, added
 * after the joiner's session-end hop was measured never returning. An arm left
 * standing would restore a stale lr into an unrelated later call. */
void lsw2_gearhop_tick();

#endif /* LSW2_GEARHOP_H */
